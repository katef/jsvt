#include "validate_ir.h"

#include "xalloc.h"
#include "jvst_macros.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include <fsm/bool.h>
#include <fsm/fsm.h>
#include <fsm/out.h>
#include <fsm/options.h>
#include <fsm/pred.h>
#include <fsm/walk.h>
#include <re/re.h>

#include "validate_sbuf.h"
#include "validate_constraints.h"
#include "sjp_testing.h"

enum { MATCH_LABEL_MAX = 4096 };

// pointers for offsets to match labels
// TODO: explain this more....
static char matchlbls[MATCH_LABEL_MAX];

const char *
jvst_invalid_msg(enum jvst_invalid_code code)
{
	switch(code) {
	case JVST_INVALID_UNEXPECTED_TOKEN:
		return "unexpected token";

	case JVST_INVALID_NOT_INTEGER:
		return "number is not an integer";

	case JVST_INVALID_NUMBER:
		return "number not valid";

	case JVST_INVALID_TOO_FEW_PROPS:
		return "too few properties";

	case JVST_INVALID_TOO_MANY_PROPS:
		return "too many properties";

	case JVST_INVALID_MISSING_REQUIRED_PROPERTIES:
		return "missing required properties";

	case JVST_INVALID_SPLIT_CONDITION:
		return "invalid split condition";

	case JVST_INVALID_BAD_PROPERTY_NAME:
		return "bad property name";
	}

	return "Unknown error";
}

enum {
	JVST_IR_STMT_CHUNKSIZE = 1024,
	JVST_IR_EXPR_CHUNKSIZE = 1024,
	JVST_IR_NUMROOTS  = 32,
};

enum {
	STMT_MARKSIZE = JVST_IR_STMT_CHUNKSIZE / CHAR_BIT +
		(JVST_IR_STMT_CHUNKSIZE % CHAR_BIT) ? 1 : 0,
};

struct jvst_ir_stmt_pool {
	struct jvst_ir_stmt_pool *next;
	struct jvst_ir_stmt items[JVST_IR_STMT_CHUNKSIZE];
	unsigned char marks[STMT_MARKSIZE];
};

static struct {
	struct jvst_ir_stmt_pool *head;
	size_t top;
	struct jvst_ir_stmt *freelist;
	struct jvst_ir_stmt **roots[JVST_IR_NUMROOTS];
	int nroots;
} stmt_pool;

static struct jvst_ir_stmt *
ir_stmt_alloc(void)
{
	struct jvst_ir_stmt *item;
	struct jvst_ir_stmt_pool *pool;

	if (stmt_pool.head == NULL) {
		goto new_pool;
	}

	if (stmt_pool.top < ARRAYLEN(stmt_pool.head->items)) {
		item = &stmt_pool.head->items[stmt_pool.top++];
		memset(item, 0, sizeof *item);
		return item;
	}

	if (stmt_pool.freelist != NULL) {
		item = stmt_pool.freelist;
		stmt_pool.freelist = stmt_pool.freelist->next;
		memset(item, 0, sizeof *item);
		return item;
	}

	// add collector here
	
new_pool:
	pool = xmalloc(sizeof *pool);
	memset(pool->items, 0, sizeof pool->items);
	memset(pool->marks, 0, sizeof pool->marks);
	pool->next = stmt_pool.head;
	stmt_pool.head = pool;
	stmt_pool.top = 1;
	return &pool->items[0];
}

static struct jvst_ir_stmt *
ir_stmt_new(enum jvst_ir_stmt_type type)
{
	struct jvst_ir_stmt *stmt;
	stmt = ir_stmt_alloc();
	stmt->type = type;

	return stmt;
}

static struct jvst_ir_stmt *
ir_stmt_invalid(enum jvst_invalid_code code)
{
	struct jvst_ir_stmt *stmt;
	stmt = ir_stmt_new(JVST_IR_STMT_INVALID);
	stmt->u.invalid.code = code;
	stmt->u.invalid.msg = jvst_invalid_msg(code); // XXX - even worth bothering with?

	return stmt;
}

static inline struct jvst_ir_stmt *
ir_stmt_valid(void)
{
	return ir_stmt_new(JVST_IR_STMT_VALID);
}

static inline struct jvst_ir_stmt *
ir_stmt_if(struct jvst_ir_expr *cond, struct jvst_ir_stmt *br_true, struct jvst_ir_stmt *br_false)
{
	struct jvst_ir_stmt *br;
	br = ir_stmt_new(JVST_IR_STMT_IF);
	br->u.if_.cond = cond;
	br->u.if_.br_true = br_true;
	br->u.if_.br_false = br_false;
	return br;
}

static inline struct jvst_ir_stmt *
ir_stmt_frame(void)
{
	struct jvst_ir_stmt *frame;
	frame = ir_stmt_new(JVST_IR_STMT_FRAME);
	frame->u.frame.nloops = 0;
	frame->u.frame.stmts = NULL;
	return frame;
}

static inline struct jvst_ir_stmt *
ir_stmt_loop(struct jvst_ir_stmt *frame, const char *loopname)
{
	struct jvst_ir_stmt *loop;

	assert(loopname != NULL);
	assert(frame != NULL);
	assert(frame->type == JVST_IR_STMT_FRAME);

	loop = ir_stmt_new(JVST_IR_STMT_LOOP);

	loop->u.loop.name = loopname;
	loop->u.loop.ind  = frame->u.frame.nloops++;
	loop->u.loop.stmts = NULL;

	return loop;
}

static inline struct jvst_ir_stmt *
ir_stmt_break(struct jvst_ir_stmt *loop)
{
	struct jvst_ir_stmt *brk;
	brk = ir_stmt_new(JVST_IR_STMT_BREAK);

	// XXX - uniquify name!
	brk->u.break_.name = loop->u.loop.name;
	brk->u.break_.ind  = loop->u.loop.ind;
	brk->u.break_.loop = loop;

	return brk;
}

static inline struct jvst_ir_stmt *
ir_stmt_counter(struct jvst_ir_stmt *frame, const char *label)
{
	struct jvst_ir_stmt *counter;

	assert(label != NULL);
	assert(frame != NULL);
	assert(frame->type == JVST_IR_STMT_FRAME);

	counter = ir_stmt_new(JVST_IR_STMT_COUNTER);

	counter->u.counter.label = label;
	counter->u.counter.ind   = frame->u.frame.ncounters++;
	counter->u.counter.frame = frame;

	counter->next = frame->u.frame.counters;
	frame->u.frame.counters = counter;

	return counter;
}

static inline struct jvst_ir_stmt *
ir_stmt_bitvec(struct jvst_ir_stmt *frame, const char *label, size_t nbits)
{
	struct jvst_ir_stmt *bitvec;

	assert(label != NULL);
	assert(frame != NULL);
	assert(frame->type == JVST_IR_STMT_FRAME);

	bitvec = ir_stmt_new(JVST_IR_STMT_BITVECTOR);

	bitvec->u.bitvec.label = label;
	bitvec->u.bitvec.ind   = frame->u.frame.nbitvecs++;
	bitvec->u.bitvec.frame = frame;
	bitvec->u.bitvec.nbits = nbits;

	bitvec->next = frame->u.frame.bitvecs;
	frame->u.frame.bitvecs = bitvec;

	return bitvec;
}

static inline struct jvst_ir_stmt *
ir_stmt_matcher(struct jvst_ir_stmt *frame, const char *name, struct fsm *dfa)
{
	struct jvst_ir_stmt *matcher;

	assert(name != NULL);
	assert(frame != NULL);
	assert(frame->type == JVST_IR_STMT_FRAME);

	matcher = ir_stmt_new(JVST_IR_STMT_MATCHER);

	matcher->u.matcher.name = name;
	matcher->u.matcher.ind  = frame->u.frame.nmatchers++;
	matcher->u.matcher.dfa  = dfa;

	matcher->next = frame->u.frame.matchers;
	frame->u.frame.matchers = matcher;

	return matcher;
}

static inline struct jvst_ir_stmt *
ir_stmt_counter_op(enum jvst_ir_stmt_type op, struct jvst_ir_stmt *counter)
{
	struct jvst_ir_stmt *opstmt;

	assert(counter->type == JVST_IR_STMT_COUNTER);
	assert((op == JVST_IR_STMT_INCR) || (op == JVST_IR_STMT_DECR));
	assert(counter != NULL);

	opstmt = ir_stmt_new(op);

	opstmt->u.counter_op.label = counter->u.counter.label;
	opstmt->u.counter_op.ind   = counter->u.counter.ind;
	opstmt->u.counter_op.counter = counter;

	return opstmt;
}

union expr_pool_item {
	union expr_pool_item *next;
	struct jvst_ir_expr expr;
};

struct jvst_ir_expr_pool {
	struct jvst_ir_expr_pool *next;
	union expr_pool_item items[JVST_IR_EXPR_CHUNKSIZE];
	unsigned char marks[STMT_MARKSIZE];
};

static struct {
	struct jvst_ir_expr_pool *head;
	size_t top;
	union expr_pool_item *freelist;
} expr_pool;

static struct jvst_ir_expr *
ir_expr_alloc(void)
{
	struct jvst_ir_expr *item;
	struct jvst_ir_expr_pool *pool;

	if (expr_pool.head == NULL) {
		goto new_pool;
	}

	if (expr_pool.top < ARRAYLEN(expr_pool.head->items)) {
		item = &expr_pool.head->items[expr_pool.top++].expr;
		memset(item, 0, sizeof *item);
		return item;
	}

	if (expr_pool.freelist != NULL) {
		item = &expr_pool.freelist->expr;
		expr_pool.freelist = expr_pool.freelist->next;
		memset(item, 0, sizeof *item);
		return item;
	}

	// add collector here
	
new_pool:
	pool = xmalloc(sizeof *pool);
	memset(pool->items, 0, sizeof pool->items);
	memset(pool->marks, 0, sizeof pool->marks);
	pool->next = expr_pool.head;
	expr_pool.head = pool;
	expr_pool.top = 1;
	return &pool->items[0].expr;
}

static struct jvst_ir_expr *
ir_expr_new(enum jvst_ir_expr_type type)
{
	struct jvst_ir_expr *expr;
	expr = ir_expr_alloc();
	expr->type = type;

	return expr;
}

static struct jvst_ir_expr *
ir_expr_num(double num)
{
	struct jvst_ir_expr *expr;
	expr = ir_expr_new(JVST_IR_EXPR_NUM);
	expr->u.vnum = num;
	return expr;
}

static struct jvst_ir_expr *
ir_expr_size(size_t sz)
{
	struct jvst_ir_expr *expr;
	expr = ir_expr_new(JVST_IR_EXPR_SIZE);
	expr->u.vsize = sz;
	return expr;
}

static struct jvst_ir_expr *
ir_expr_bool(int v)
{
	struct jvst_ir_expr *expr;
	expr = ir_expr_new(JVST_IR_EXPR_BOOL);
	expr->u.vbool = !!v;
	return expr;
}

static struct jvst_ir_expr *
ir_expr_op(enum jvst_ir_expr_type op,
	struct jvst_ir_expr *left, struct jvst_ir_expr *right)
{
	struct jvst_ir_expr *expr;

	switch (op) {
	case JVST_IR_EXPR_AND:
	case JVST_IR_EXPR_OR:
		expr = ir_expr_new(op);
		expr->u.and_or.left = left;
		expr->u.and_or.right = right;
		break;

	case JVST_IR_EXPR_NE:
	case JVST_IR_EXPR_LT:
	case JVST_IR_EXPR_LE:
	case JVST_IR_EXPR_EQ:
	case JVST_IR_EXPR_GE:
	case JVST_IR_EXPR_GT:
		expr = ir_expr_new(op);
		expr->u.cmp.left = left;
		expr->u.cmp.right = right;
		break;

	case JVST_IR_EXPR_NONE:
	case JVST_IR_EXPR_NUM:
	case JVST_IR_EXPR_SIZE:
	case JVST_IR_EXPR_BOOL:
	case JVST_IR_EXPR_TOK_TYPE:
	case JVST_IR_EXPR_TOK_NUM:
	case JVST_IR_EXPR_TOK_COMPLETE:
	case JVST_IR_EXPR_TOK_LEN:
	case JVST_IR_EXPR_COUNT:
	case JVST_IR_EXPR_BTEST:
	case JVST_IR_EXPR_BTESTALL:
	case JVST_IR_EXPR_BTESTANY:
	case JVST_IR_EXPR_BTESTONE:
	case JVST_IR_EXPR_BCOUNT:
	case JVST_IR_EXPR_ISTOK:
	case JVST_IR_EXPR_ISINT:
	case JVST_IR_EXPR_NOT:
	case JVST_IR_EXPR_SPLIT:
		fprintf(stderr, "invalid OP type: %s\n", jvst_ir_expr_type_name(op));
		abort();
	}

	return expr;
}

static struct jvst_ir_expr *
ir_expr_istok(enum SJP_EVENT tt)
{
	struct jvst_ir_expr *expr;
	expr = ir_expr_new(JVST_IR_EXPR_ISTOK);
	expr->u.istok.tok_type = tt;

	return expr;
}

static struct jvst_ir_expr *
ir_expr_count(struct jvst_ir_stmt *counter)
{
	struct jvst_ir_expr *expr;

	assert(counter->type == JVST_IR_STMT_COUNTER);

	expr = ir_expr_new(JVST_IR_EXPR_COUNT);
	expr->u.count.counter = counter;
	expr->u.count.label = counter->u.counter.label;
	expr->u.count.ind   = counter->u.counter.ind;

	return expr;
}



/** mcase pooling **/

struct jvst_ir_mcase_pool {
	struct jvst_ir_mcase_pool *next;
	struct jvst_ir_mcase items[JVST_IR_EXPR_CHUNKSIZE];
	unsigned char marks[STMT_MARKSIZE];
};

static struct {
	struct jvst_ir_mcase_pool *head;
	size_t top;
	struct jvst_ir_mcase *freelist;
} mcase_pool;

static struct jvst_ir_mcase *
ir_mcase_alloc(void)
{
	struct jvst_ir_mcase *item;
	struct jvst_ir_mcase_pool *pool;

	if (mcase_pool.head == NULL) {
		goto new_pool;
	}

	if (mcase_pool.top < ARRAYLEN(mcase_pool.head->items)) {
		item = &mcase_pool.head->items[mcase_pool.top++];
		memset(item, 0, sizeof *item);
		return item;
	}

	if (mcase_pool.freelist != NULL) {
		item = mcase_pool.freelist;
		mcase_pool.freelist = mcase_pool.freelist->next;
		memset(item, 0, sizeof *item);
		return item;
	}

	// XXX - add collector here

new_pool:
	pool = xmalloc(sizeof *pool);
	memset(pool->items, 0, sizeof pool->items);
	memset(pool->marks, 0, sizeof pool->marks);
	pool->next = mcase_pool.head;
	mcase_pool.head = pool;
	mcase_pool.top = 1;
	return &pool->items[0];
}

static struct jvst_ir_mcase *
ir_mcase_new(size_t which, struct jvst_ir_stmt *stmt)
{
	struct jvst_ir_mcase *mcase;

	mcase = ir_mcase_alloc();
	mcase->which = which;
	mcase->stmt = stmt;

	return mcase;
}

const char *
jvst_ir_stmt_type_name(enum jvst_ir_stmt_type type)
{
	switch (type) {
	case JVST_IR_STMT_INVALID:
		return "INVALID";
	case JVST_IR_STMT_NOP:	
		return "NOP";
	case JVST_IR_STMT_VALID:		
		return "VALID";
	case JVST_IR_STMT_IF:
		return "IF";
	case JVST_IR_STMT_LOOP:
		return "LOOP";
	case JVST_IR_STMT_SEQ:
		return "SEQ";
	case JVST_IR_STMT_BREAK:
		return "BREAK";
	case JVST_IR_STMT_TOKEN:
		return "TOKEN";    	
	case JVST_IR_STMT_CONSUME:
		return "CONSUME";  	
	case JVST_IR_STMT_FRAME:
		return "FRAME";    	
	case JVST_IR_STMT_COUNTER:
		return "COUNTER";  	
	case JVST_IR_STMT_MATCHER:
		return "MATCHER";  	
	case JVST_IR_STMT_BITVECTOR:
		return "BITVECTOR";	
	case JVST_IR_STMT_BSET:
		return "BSET";
	case JVST_IR_STMT_BCLEAR:
		return "BCLEAR";   	
	case JVST_IR_STMT_INCR:
		return "INCR";
	case JVST_IR_STMT_DECR:
		return "DECR";
	case JVST_IR_STMT_MATCH:
		return "MATCH";    	

	default:
		fprintf(stderr, "%s:%d unknown IR statement type %d in %s\n",
			__FILE__, __LINE__, type, __func__);
		abort();
	}
}

const char *
jvst_ir_expr_type_name(enum jvst_ir_expr_type type)
{

	switch (type) {
	case JVST_IR_EXPR_NONE:
		return "NONE";

	case JVST_IR_EXPR_TOK_TYPE:
		return "TOK_TYPE";

	case JVST_IR_EXPR_TOK_NUM:
		return "TOK_NUM";

	case JVST_IR_EXPR_TOK_COMPLETE:
		return "TOK_COMPLETE";

	case JVST_IR_EXPR_TOK_LEN:
		return "TOK_LEN";

	case JVST_IR_EXPR_COUNT:
		return "COUNT";

	case JVST_IR_EXPR_BTEST:
		return "BTEST";

	case JVST_IR_EXPR_BTESTALL:
		return "BTESTALL";

	case JVST_IR_EXPR_BTESTANY:
		return "BTESTANY";

	case JVST_IR_EXPR_BTESTONE:
		return "BTESTONE";

	case JVST_IR_EXPR_BCOUNT:
		return "BCOUNT";

	case JVST_IR_EXPR_ISTOK:
		return "ISTOK";

	case JVST_IR_EXPR_AND:
		return "AND";

	case JVST_IR_EXPR_OR:
		return "OR";

	case JVST_IR_EXPR_NOT:
		return "NOT";

	case JVST_IR_EXPR_NE:
		return "NE";

	case JVST_IR_EXPR_LT:
		return "LT";

	case JVST_IR_EXPR_LE:
		return "LE";

	case JVST_IR_EXPR_EQ:
		return "EQ";

	case JVST_IR_EXPR_GE:
		return "GE";

	case JVST_IR_EXPR_GT:
		return "GT";

	case JVST_IR_EXPR_ISINT:
		return "ISINT";

	case JVST_IR_EXPR_SPLIT:
		return "SPLIT";

	default:
		fprintf(stderr, "%s:%d unknown IR expression node type %d in %s\n",
			__FILE__, __LINE__, type, __func__);
		abort();
	}
}

void
jvst_ir_dump_inner(struct sbuf *buf, struct jvst_ir_stmt *ir, int indent);

static
void dump_stmt_list_inner(struct sbuf *buf, struct jvst_ir_stmt *stmts, int indent)
{
	for (;stmts != NULL; stmts = stmts->next) {
		jvst_ir_dump_inner(buf, stmts, indent+2);
		if (stmts->next != NULL) {
			sbuf_snprintf(buf, ",\n");
		} else {
			sbuf_snprintf(buf, "\n");
		}
	}
}

static
void dump_stmt_list(struct sbuf *buf, enum jvst_ir_stmt_type type, struct jvst_ir_stmt *stmts, int indent)
{
	if (stmts == NULL) {
		sbuf_snprintf(buf, "%s()", jvst_ir_stmt_type_name(type));
		return;
	}

	sbuf_snprintf(buf, "%s(\n", jvst_ir_stmt_type_name(type));
	dump_stmt_list_inner(buf, stmts, indent);
	sbuf_indent(buf, indent);
	sbuf_snprintf(buf, ")");
}

void
jvst_ir_dump_expr(struct sbuf *buf, struct jvst_ir_expr *expr, int indent)
{
	sbuf_indent(buf, indent);
	switch (expr->type) {
	case JVST_IR_EXPR_NONE:
	case JVST_IR_EXPR_TOK_TYPE:
	case JVST_IR_EXPR_TOK_NUM:
	case JVST_IR_EXPR_TOK_COMPLETE:
	case JVST_IR_EXPR_TOK_LEN:
		sbuf_snprintf(buf, "%s", jvst_ir_expr_type_name(expr->type));
		break;

	case JVST_IR_EXPR_ISTOK:
		sbuf_snprintf(buf, "ISTOK($%s)",
				evt2name(expr->u.istok.tok_type));
		break;

	case JVST_IR_EXPR_NUM:
		sbuf_snprintf(buf, "%.1f", expr->u.vnum);
		break;

	case JVST_IR_EXPR_SIZE:
		sbuf_snprintf(buf, "%zu", expr->u.vsize);
		break;

	case JVST_IR_EXPR_BOOL:
		sbuf_snprintf(buf, "%s", expr->u.vbool ? "TRUE" : "FALSE" );
		break;

	case JVST_IR_EXPR_AND:
	case JVST_IR_EXPR_OR:
		{
			const char *op = (expr->type == JVST_IR_EXPR_AND) ? "AND" : "OR";
			sbuf_snprintf(buf, "%s(\n",op);
			jvst_ir_dump_expr(buf,expr->u.and_or.left,indent+2);
			sbuf_snprintf(buf, ",\n");
			jvst_ir_dump_expr(buf,expr->u.and_or.right,indent+2);
			sbuf_snprintf(buf, "\n");
			sbuf_indent(buf, indent);
			sbuf_snprintf(buf, ")");
		}
		break;

	case JVST_IR_EXPR_NOT:
		{
			sbuf_snprintf(buf, "NOT(\n");
			jvst_ir_dump_expr(buf,expr->u.and_or.left,indent+2);
			sbuf_snprintf(buf, ",\n");
			jvst_ir_dump_expr(buf,expr->u.and_or.right,indent+2);
			sbuf_indent(buf, indent);
			sbuf_snprintf(buf, ")");
		}
		break;

	case JVST_IR_EXPR_NE:
	case JVST_IR_EXPR_LT:
	case JVST_IR_EXPR_LE:
	case JVST_IR_EXPR_EQ:
	case JVST_IR_EXPR_GE:
	case JVST_IR_EXPR_GT:
		{
			const char *op = jvst_ir_expr_type_name(expr->type);
			sbuf_snprintf(buf, "%s(\n", op);
			jvst_ir_dump_expr(buf,expr->u.and_or.left,indent+2);
			sbuf_snprintf(buf, ",\n");
			jvst_ir_dump_expr(buf,expr->u.and_or.right,indent+2);
			sbuf_snprintf(buf, "\n");
			sbuf_indent(buf, indent);
			sbuf_snprintf(buf, ")");
		}
		break;

	case JVST_IR_EXPR_ISINT:
		sbuf_snprintf(buf, "ISINT(\n");
		jvst_ir_dump_expr(buf,expr->u.isint.arg,indent+2);
		sbuf_snprintf(buf, "\n");
		sbuf_indent(buf, indent);
		sbuf_snprintf(buf, ")");
		break;

	case JVST_IR_EXPR_SPLIT:
		{
			struct jvst_ir_stmt *stmts;
			stmts = expr->u.split.frames;
			if (stmts == NULL) {
				sbuf_snprintf(buf, "SPLIT()");
			} else {
				sbuf_snprintf(buf, "SPLIT(\n");
				for (;stmts != NULL; stmts = stmts->next) {
					jvst_ir_dump_inner(buf, stmts, indent+2);
					if (stmts->next != NULL) {
						sbuf_snprintf(buf, ",\n");
					} else {
						sbuf_snprintf(buf, "\n");
					}
				}
				sbuf_indent(buf, indent);
				sbuf_snprintf(buf, ")");
			}
		}
		break;

	case JVST_IR_EXPR_COUNT:
		sbuf_snprintf(buf, "COUNT(%zu, \"%s_%zu\")",
			expr->u.count.ind,
			expr->u.count.label,
			expr->u.count.ind);
		break;

	case JVST_IR_EXPR_BTEST:
	case JVST_IR_EXPR_BTESTANY:
	case JVST_IR_EXPR_BTESTALL:
	case JVST_IR_EXPR_BTESTONE:
	case JVST_IR_EXPR_BCOUNT:
		sbuf_snprintf(buf, "%s(%zu, \"%s_%zu\", b0=%zu, b1=%zu)",
			jvst_ir_expr_type_name(expr->type),
			expr->u.btest.ind,
			expr->u.btest.label,
			expr->u.btest.ind,
			expr->u.btest.b0,
			expr->u.btest.b1);
		break;

	default:
		fprintf(stderr, "unknown IR expression type %d\n", expr->type);
		abort();
	}


}

// definition in validate_constraints.c
void
jvst_cnode_matchset_dump(struct jvst_cnode_matchset *ms, struct sbuf *buf, int indent);

void
jvst_ir_dump_inner(struct sbuf *buf, struct jvst_ir_stmt *ir, int indent)
{
	assert(ir != NULL);

	sbuf_indent(buf, indent);
	switch (ir->type) {
	case JVST_IR_STMT_INVALID:	
		sbuf_snprintf(buf,
			"INVALID(%d, \"%s\")",
			ir->u.invalid.code,
			ir->u.invalid.msg);
		break;

	case JVST_IR_STMT_NOP:	
	case JVST_IR_STMT_VALID:		
	case JVST_IR_STMT_TOKEN:		
	case JVST_IR_STMT_CONSUME:		
		sbuf_snprintf(buf, "%s", jvst_ir_stmt_type_name(ir->type));
		break;

	case JVST_IR_STMT_SEQ:
		dump_stmt_list(buf, ir->type, ir->u.stmt_list, indent);
		break;

	case JVST_IR_STMT_FRAME:		
		{
			assert(ir->u.frame.stmts != NULL);
			sbuf_snprintf(buf, "FRAME(\n");

			if (ir->u.frame.counters) {
				sbuf_indent(buf, indent+2);
				sbuf_snprintf(buf, "COUNTERS[\n");
				dump_stmt_list_inner(buf, ir->u.frame.counters, indent+2);
				sbuf_indent(buf, indent+2);
				sbuf_snprintf(buf, "],\n");
			}

			if (ir->u.frame.matchers) {
				sbuf_indent(buf, indent+2);
				sbuf_snprintf(buf, "MATCHERS[\n");
				dump_stmt_list_inner(buf, ir->u.frame.matchers, indent+2);
				sbuf_indent(buf, indent+2);
				sbuf_snprintf(buf, "],\n");
			}

			if (ir->u.frame.bitvecs) {
				sbuf_indent(buf, indent+2);
				sbuf_snprintf(buf, "BITVECS[\n");
				dump_stmt_list_inner(buf, ir->u.frame.bitvecs, indent+2);
				sbuf_indent(buf, indent+2);
				sbuf_snprintf(buf, "],\n");
			}

			dump_stmt_list_inner(buf, ir->u.frame.stmts, indent);

			sbuf_indent(buf, indent);
			sbuf_snprintf(buf, ")");
		}
		break;

	case JVST_IR_STMT_LOOP:		
		{
			assert(ir->u.loop.stmts != NULL);
			sbuf_snprintf(buf, "LOOP(\"%s\",\n",
				ir->u.loop.name);
			dump_stmt_list_inner(buf, ir->u.loop.stmts, indent);
			sbuf_indent(buf, indent);
			sbuf_snprintf(buf, ")");
		}
		break;

	case JVST_IR_STMT_IF:
		sbuf_snprintf(buf, "IF(\n");
		jvst_ir_dump_expr(buf, ir->u.if_.cond, indent+2);
		sbuf_snprintf(buf, ",\n");
		jvst_ir_dump_inner(buf, ir->u.if_.br_true, indent+2);
		sbuf_snprintf(buf, ",\n");
		jvst_ir_dump_inner(buf, ir->u.if_.br_false, indent+2);
		sbuf_snprintf(buf, "\n");
		sbuf_indent(buf, indent);
		sbuf_snprintf(buf, ")");
		break;

	case JVST_IR_STMT_MATCHER:
		sbuf_snprintf(buf, "MATCHER(%zu, \"%s_%zu\")",
			ir->u.matcher.ind, ir->u.matcher.name, ir->u.matcher.ind);
		break;

	case JVST_IR_STMT_BREAK:
		sbuf_snprintf(buf, "BREAK(\"%s_%zu\")", ir->u.break_.name, ir->u.break_.ind);
		break;

	case JVST_IR_STMT_MATCH:
		{
			struct jvst_ir_mcase *cases;
			sbuf_snprintf(buf, "MATCH(%zu,\n", ir->u.match.ind);

			if (ir->u.match.default_case != NULL) {
				sbuf_indent(buf, indent+2);
				sbuf_snprintf(buf, "DEFAULT_CASE(\n");

				jvst_ir_dump_inner(buf, ir->u.match.default_case, indent+4);
				sbuf_snprintf(buf, "\n");

				sbuf_indent(buf, indent+2);
				if (ir->u.match.cases != NULL) {
					sbuf_snprintf(buf, "),\n");
				} else {
					sbuf_snprintf(buf, ")\n");
				}
			}

			for (cases = ir->u.match.cases; cases != NULL; cases = cases->next) {
				struct jvst_cnode_matchset *mset;

				sbuf_indent(buf, indent+2);
				sbuf_snprintf(buf, "CASE(%zu,\n", cases->which);

				for (mset=cases->matchset; mset != NULL; mset = mset->next) {
					jvst_cnode_matchset_dump(mset, buf, indent+4);
					sbuf_snprintf(buf, ",\n");
				}

				jvst_ir_dump_inner(buf, cases->stmt, indent+4);
				sbuf_snprintf(buf, "\n");

				sbuf_indent(buf, indent+2);
				sbuf_snprintf(buf, ")");
				if (cases->next) {
					sbuf_snprintf(buf, ",\n");
				} else {
					sbuf_snprintf(buf, "\n");
				}
			}
			sbuf_indent(buf, indent);
			sbuf_snprintf(buf, ")");
		}
		break;

	case JVST_IR_STMT_COUNTER:		
		sbuf_snprintf(buf, "COUNTER(%zu, \"%s_%zu\")",
			ir->u.counter.ind, ir->u.counter.label, ir->u.counter.ind);
		break;

	case JVST_IR_STMT_INCR:		
	case JVST_IR_STMT_DECR:		
		sbuf_snprintf(buf, "%s(%zu, \"%s_%zu\")",
			jvst_ir_stmt_type_name(ir->type),
			ir->u.counter_op.ind,
			ir->u.counter_op.label,
			ir->u.counter_op.ind);
		break;

	case JVST_IR_STMT_BSET:
	case JVST_IR_STMT_BCLEAR:
		sbuf_snprintf(buf, "%s(%zu, \"%s_%zu\", bit=%zu)",
			jvst_ir_stmt_type_name(ir->type),
			ir->u.bitop.ind,
			ir->u.bitop.label,
			ir->u.bitop.ind,
			ir->u.bitop.bit);
		break;

	case JVST_IR_STMT_BITVECTOR:		
		sbuf_snprintf(buf, "%s(%zu, \"%s_%zu\", nbits=%zu)",
			jvst_ir_stmt_type_name(ir->type),
			ir->u.bitvec.ind,
			ir->u.bitvec.label,
			ir->u.bitvec.ind,
			ir->u.bitvec.nbits);
		break;

	case JVST_IR_STMT_SPLITVEC:
		{
			sbuf_snprintf(buf, "SPLITVEC(%zu, \"%s_%zu\",\n",
				ir->u.splitvec.ind,
				ir->u.splitvec.label,
				ir->u.splitvec.ind);

			assert(ir->u.splitvec.split_frames != NULL);
			dump_stmt_list_inner(buf, ir->u.splitvec.split_frames, indent);
			sbuf_indent(buf, indent);
			sbuf_snprintf(buf, ")");
		}
		break;

	default:
		fprintf(stderr, "%s:%d unknown IR statement type %d in %s\n",
			__FILE__, __LINE__, ir->type, __func__);
		abort();
	}
}

int
jvst_ir_dump(struct jvst_ir_stmt *ir, char *buf, size_t nb)
{
	struct sbuf b = {
	    .buf = buf, .cap = nb, .len = 0, .np = 0,
	};

	jvst_ir_dump_inner(&b, ir, 0);
	sbuf_snprintf(&b, "\n");
	return (b.len < b.cap) ? 0 : -1;
}

static struct jvst_ir_stmt *
ir_translate_number(struct jvst_cnode *top)
{
	struct jvst_ir_stmt *stmt, **spp;
	// struct jvst_ir_expr *expr, **epp;

	stmt = NULL;
	spp = &stmt;

	switch (top->type) {
	case JVST_CNODE_VALID:
		*spp = ir_stmt_new(JVST_IR_STMT_VALID);
		break;

	case JVST_CNODE_INVALID:
		*spp = ir_stmt_invalid(JVST_INVALID_UNEXPECTED_TOKEN);
		break;

	case JVST_CNODE_NUM_INTEGER:
		{
			struct jvst_ir_stmt *br;
			struct jvst_ir_expr *cond;
			cond = ir_expr_new(JVST_IR_EXPR_ISINT);
			cond->u.isint.arg = ir_expr_new(JVST_IR_EXPR_TOK_NUM);

			br = ir_stmt_new(JVST_IR_STMT_IF);
			br->u.if_.cond = cond;
			br->u.if_.br_true = ir_stmt_new(JVST_IR_STMT_VALID);
			br->u.if_.br_false = ir_stmt_invalid(JVST_INVALID_NOT_INTEGER);

			*spp = br;
		}
		break;

	case JVST_CNODE_NUM_RANGE:
		{
			struct jvst_ir_stmt *br;
			struct jvst_ir_expr *cond, *lower, *upper;

			lower = NULL;
			upper = NULL;
			if (top->u.num_range.flags & JVST_CNODE_RANGE_EXCL_MIN) {
				lower = ir_expr_op(JVST_IR_EXPR_GT,
						ir_expr_new(JVST_IR_EXPR_TOK_NUM),
						ir_expr_num(top->u.num_range.min));
			} else if (top->u.num_range.flags & JVST_CNODE_RANGE_MIN) {
				lower = ir_expr_op(JVST_IR_EXPR_GE,
						ir_expr_new(JVST_IR_EXPR_TOK_NUM),
						ir_expr_num(top->u.num_range.min));
			}

			if (top->u.num_range.flags & JVST_CNODE_RANGE_EXCL_MAX) {
				upper = ir_expr_op(JVST_IR_EXPR_LT,
						ir_expr_new(JVST_IR_EXPR_TOK_NUM),
						ir_expr_num(top->u.num_range.min));
			} else if (top->u.num_range.flags & JVST_CNODE_RANGE_MAX) {
				upper = ir_expr_op(JVST_IR_EXPR_LE,
						ir_expr_new(JVST_IR_EXPR_TOK_NUM),
						ir_expr_num(top->u.num_range.min));
			}

			assert((lower != NULL) || (upper != NULL));

			if (lower && upper) {
				cond = ir_expr_op(JVST_IR_EXPR_AND, lower, upper);
			} else if (lower) {
				cond = lower;
			} else {
				cond = upper;
			}

			br = ir_stmt_if(cond,
				ir_stmt_new(JVST_IR_STMT_VALID),
				ir_stmt_invalid(JVST_INVALID_NUMBER));
			*spp = br;
		}
		break;

	case JVST_CNODE_AND:
	case JVST_CNODE_OR:
	case JVST_CNODE_NOT:
	case JVST_CNODE_XOR:
		fprintf(stderr, "[%s:%d] cnode %s not yet implemented\n",
				__FILE__, __LINE__, 
				jvst_cnode_type_name(top->type));
		abort();

	default:
		fprintf(stderr, "[%s:%d] invalid cnode type %s for $NUMBER\n",
				__FILE__, __LINE__, 
				jvst_cnode_type_name(top->type));
		abort();
	}

	return stmt;
}

static void
merge_constraints(const struct fsm_state **orig, size_t n,
	struct fsm *dfa, struct fsm_state *comb)
{
	struct jvst_ir_mcase *mcase;
	size_t i;

	fprintf(stderr, "... MERGING CONSTRAINTS ... \n");
	for (mcase = NULL, i = 0; i < n; i++) {
		struct jvst_ir_mcase *c;

		if (!fsm_isend(dfa, orig[i])) {
			continue;
		}

		c = fsm_getopaque(dfa, orig[i]);
		if (c == NULL) {
			fprintf(stderr, "case is NULL!\n");
			continue;
		}

		fprintf(stderr, "merging case %p %zu\n", (void *)c, c->which);
	}

	for (mcase = NULL, i = 0; i < n; i++) {
		struct jvst_ir_mcase *newcase;
		struct jvst_ir_stmt *seq;

		if (!fsm_isend(dfa, orig[i])) {
			continue;
		}

		newcase = fsm_getopaque(dfa, orig[i]);
		if (mcase == NULL) {
			assert(newcase->stmt != NULL);
			mcase = newcase;
			continue;
		}

		if (newcase->stmt == NULL || mcase == newcase) {
			continue;
		}

		// if necessary, convert mcase statement to SEQ
		if (mcase->stmt->type == JVST_IR_STMT_SEQ) {
			seq = mcase->stmt;
		} else {
			seq = ir_stmt_new(JVST_IR_STMT_SEQ);
			seq->u.stmt_list = mcase->stmt;
			mcase->stmt = seq;
		}

		// have to append FRAMEs, but we can prepend everything
		// else...
		if (newcase->stmt->type == JVST_IR_STMT_FRAME) {
			struct jvst_ir_stmt **spp;
			for(spp = &seq->u.stmt_list; *spp != NULL; spp = &(*spp)->next) {
				continue;
			}

			*spp = newcase->stmt;
		} else {
			struct jvst_ir_stmt *stmt;
			stmt = newcase->stmt;
			assert(stmt->next == NULL);
			stmt->next = seq->u.stmt_list;
			seq->u.stmt_list = stmt;
		}

		newcase->stmt = NULL;  // so we don't process it twice (is this possible?)

		// NB: we have to remove cases where stmt==NULL after
		// the NFAs are merged
	}

	if (mcase != NULL) {
		fsm_setopaque(dfa, comb, mcase);
	}
}

#define UNASSIGNED_MATCH  (~(size_t)0)

struct ir_object_builder {
	struct jvst_ir_stmt *frame;
	struct jvst_ir_stmt *oloop;
	struct jvst_ir_stmt *match;
	struct jvst_ir_stmt **pre_loop;
	struct jvst_ir_stmt **post_loop;
	struct jvst_ir_stmt **pre_match;
	struct jvst_ir_stmt **post_match;
	struct jvst_ir_mcase **mcpp;
	struct jvst_ir_stmt *reqmask;
	struct fsm *matcher;

	bool consumed;
};

static struct jvst_ir_stmt *
obj_mcase_translate_inner(struct jvst_cnode *ctree, struct ir_object_builder *builder)
{
	switch (ctree->type) {
	case JVST_CNODE_OBJ_REQBIT:
		{
			struct jvst_ir_stmt *setbit;

			assert(builder->reqmask != NULL);

			setbit = ir_stmt_new(JVST_IR_STMT_BSET);
			setbit->u.bitop.frame = builder->reqmask->u.bitvec.frame;
			setbit->u.bitop.label = builder->reqmask->u.bitvec.label;
			setbit->u.bitop.ind   = builder->reqmask->u.bitvec.ind;
			setbit->u.bitop.bit   = ctree->u.reqbit.bit;

			return setbit;
		}

	case JVST_CNODE_VALID:
		builder->consumed = true;
		return ir_stmt_new(JVST_IR_STMT_VALID);

	case JVST_CNODE_INVALID:
		// XXX - better error message!
		builder->consumed = true;
		return ir_stmt_invalid(JVST_INVALID_BAD_PROPERTY_NAME);

	default:
		builder->consumed = true; // XXX - is this correct?
		return jvst_ir_translate(ctree);
	}
}

static struct jvst_ir_stmt *
obj_mcase_translate(struct jvst_cnode *ctree, struct ir_object_builder *builder)
{
	struct jvst_ir_stmt *stmt, **spp;
	struct jvst_cnode *n;

	if (ctree->type != JVST_CNODE_AND) {
		return obj_mcase_translate_inner(ctree,builder);
	}

	stmt = ir_stmt_new(JVST_IR_STMT_SEQ);
	spp = &stmt->u.stmt_list;
	for(n=ctree->u.ctrl; n != NULL; n=n->next) {
		*spp = obj_mcase_translate_inner(n,builder);
		spp = &(*spp)->next;
	}

	return stmt;
}

static struct ir_object_builder *obj_mcase_builder_state;

static int
obj_mcase_builder(const struct fsm *dfa, const struct fsm_state *st)
{
	struct jvst_cnode *node;
	struct jvst_ir_mcase *mcase;
	struct jvst_ir_stmt *stmt;

	if (!fsm_isend(dfa, st)) {
		return 1;
	}

	node = fsm_getopaque((struct fsm *)dfa, st);
	assert(node->type == JVST_CNODE_MATCH_CASE);
	assert(node->u.mcase.tmp == NULL);

	// XXX - this is a hack
	// Basically, we need to keep track of whether the property
	// value is consumed or not, and add a CONSUME statement after
	// translation if it isn't.
	obj_mcase_builder_state->consumed = false;
	stmt = obj_mcase_translate(node->u.mcase.constraint, obj_mcase_builder_state);
	if (!obj_mcase_builder_state->consumed) {
		struct jvst_ir_stmt *seq, **spp;
		seq = ir_stmt_new(JVST_IR_STMT_SEQ);
		spp = &seq->u.stmt_list;
		*spp = stmt;
		spp = &(*spp)->next;
		*spp = ir_stmt_new(JVST_IR_STMT_CONSUME);
		
		stmt = seq;
	}
	mcase = ir_mcase_new(UNASSIGNED_MATCH, stmt);
	mcase->matchset = node->u.mcase.matchset;

	node->u.mcase.tmp = mcase;
	fsm_setopaque((struct fsm *)dfa, (struct fsm_state *)st, mcase);

	return 1;
}

struct jvst_ir_stmt *
obj_default_case(void)
{
	return ir_stmt_new(JVST_IR_STMT_CONSUME);
}

static void
ir_translate_obj_inner(struct jvst_cnode *top, struct ir_object_builder *builder)
{
	// descend the cnode tree and handle various events
	switch (top->type) {
	case JVST_CNODE_VALID:
	case JVST_CNODE_INVALID:
		// VALID/INVALID should have been picked up in the
		// various cases...
		fprintf(stderr, "top node should not be VALID or INVALID\n");
		abort();
		return;

	case JVST_CNODE_OBJ_REQUIRED:
	case JVST_CNODE_OBJ_PROP_SET:
		fprintf(stderr, "canonified cnode trees should not have %s nodes\n",
			jvst_cnode_type_name(top->type));
		abort();
		return;

	case JVST_CNODE_MATCH_SWITCH:
		{
			size_t which;
			struct jvst_cnode *caselist;
			struct jvst_ir_stmt *frame, **spp, *matcher_stmt;

			// duplicate DFA.
			builder->matcher = fsm_clone(top->u.mswitch.dfa);

			// replace MATCH_CASE opaque entries in copy with jvst_ir_mcase nodes
			obj_mcase_builder_state = builder;
			fsm_all(builder->matcher, obj_mcase_builder);
			obj_mcase_builder_state = NULL;

			// assemble jvst_ir_mcase nodes into list for an MATCH_SWITCH node and number the cases
			which = 0;
			for (caselist = top->u.mswitch.cases; caselist != NULL; caselist = caselist->next) {
				struct jvst_ir_mcase *mc;
				assert(caselist->type == JVST_CNODE_MATCH_CASE);
				assert(caselist->u.mcase.tmp != NULL);

				mc = caselist->u.mcase.tmp;
				caselist->u.mcase.tmp = NULL;
				assert(mc->next == NULL);

				mc->which = ++which;
				*builder->mcpp = mc;
				builder->mcpp = &mc->next;
			}

			// 4. translate the default case
			//
			// Currently we do nothing, because the
			// default_case is always VALID.
			//
			// FIXME: is default_case always VALID?  in that
			// case we can eliminate it.  Otherwise, we need
			// to do something more sophisticated here.

			// 5. Add matcher statement to frame and fixup refs
			matcher_stmt = ir_stmt_matcher(builder->frame, "dfa", builder->matcher);
			builder->match->u.match.dfa = builder->matcher;
			builder->match->u.match.name = matcher_stmt->u.matcher.name;
			builder->match->u.match.ind  = matcher_stmt->u.matcher.ind;
		}
		return;

	case JVST_CNODE_COUNT_RANGE:
		{
			struct jvst_ir_stmt *counter, *check, **checkpp;
			struct jvst_ir_expr *check_expr;
			// 1. allocate a counter to keep track of the
			//    number of properties

			counter = ir_stmt_counter(builder->frame, "num_props");

			// 2. post-match, increment the counter
			assert(builder->post_match != NULL);
			assert(*builder->post_match == NULL);
			*builder->post_match = ir_stmt_counter_op(JVST_IR_STMT_INCR, counter);
			builder->post_match = &(*builder->post_match)->next;

			// 3. post-loop check that the counter is within
			// range
			assert(builder->post_loop != NULL);
			assert(*builder->post_loop == NULL);

			checkpp = builder->post_loop;
			if (top->u.counts.min > 0) {
				*checkpp = ir_stmt_if(
					ir_expr_op(JVST_IR_EXPR_GE,
						ir_expr_count(counter),
						ir_expr_size(top->u.counts.min)),
					NULL,
					ir_stmt_invalid(JVST_INVALID_TOO_FEW_PROPS));
				checkpp = &(*checkpp)->u.if_.br_true;
			}

			if (top->u.counts.max > 0) {
				*checkpp = ir_stmt_if(
					ir_expr_op(JVST_IR_EXPR_LE,
						ir_expr_count(counter),
						ir_expr_size(top->u.counts.max)),
					NULL,
					ir_stmt_invalid(JVST_INVALID_TOO_MANY_PROPS));
				checkpp = &(*checkpp)->u.if_.br_true;
			}

			builder->post_loop = checkpp;
		}
		return;

	case JVST_CNODE_OBJ_REQMASK:
		{
			struct jvst_ir_stmt *bitvec, **checkpp;
			struct jvst_ir_expr *allbits;

			// cnode simplification should ensure that we
			// have only one reqmask per object!
			assert(builder->reqmask == NULL);

			// 1. allocate bitvector
			bitvec = ir_stmt_bitvec(builder->frame, "reqmask", top->u.reqmask.nbits);
			builder->reqmask = bitvec;

			// 2. post-loop check that all bits of bitvector
			//    are set

			allbits = ir_expr_new(JVST_IR_EXPR_BTESTALL);
			allbits->u.btest.frame = bitvec->u.bitvec.frame;
			allbits->u.btest.label = bitvec->u.bitvec.label;
			allbits->u.btest.ind   = bitvec->u.bitvec.ind;
			allbits->u.btest.b0 = 0;
			allbits->u.btest.b1 = -1;

			checkpp = builder->post_loop;
			*checkpp = ir_stmt_if(allbits,
					NULL,
					ir_stmt_invalid(JVST_INVALID_MISSING_REQUIRED_PROPERTIES));
			checkpp = &(*checkpp)->u.if_.br_true;

			builder->post_loop = checkpp;
		}
		return;

	case JVST_CNODE_AND:
		{
			struct jvst_cnode *n;
			for (n = top->u.ctrl; n != NULL; n = n->next) {
				ir_translate_obj_inner(n, builder);
			}
		}
		return;

	case JVST_CNODE_OR:
	case JVST_CNODE_NOT:
	case JVST_CNODE_XOR:
		fprintf(stderr, "[%s:%d] cnode %s not yet implemented\n",
				__FILE__, __LINE__, 
				jvst_cnode_type_name(top->type));
		abort();
		return;

	case JVST_CNODE_SWITCH:
	case JVST_CNODE_OBJ_PROP_MATCH:
	case JVST_CNODE_MATCH_CASE:
	case JVST_CNODE_OBJ_REQBIT:
		fprintf(stderr, "[%s:%d] invalid cnode type %s while handling properties of an OBJECT\n",
				__FILE__, __LINE__, 
				jvst_cnode_type_name(top->type));
		abort();

	case JVST_CNODE_ARR_ITEM:
	case JVST_CNODE_ARR_ADDITIONAL:
	case JVST_CNODE_ARR_UNIQUE:
	case JVST_CNODE_STR_MATCH:
	case JVST_CNODE_NUM_RANGE:
	case JVST_CNODE_NUM_INTEGER:
		fprintf(stderr, "[%s:%d] invalid cnode type %s for OBJECT\n",
				__FILE__, __LINE__, 
				jvst_cnode_type_name(top->type));
		abort();
	}

	fprintf(stderr, "[%s:%d] unknown cnode type %d\n",
			__FILE__, __LINE__, top->type);
	abort();
}

static struct jvst_ir_stmt *
ir_translate_object(struct jvst_cnode *top, struct jvst_ir_stmt *frame);

static struct jvst_ir_stmt *
ir_translate_object_inner(struct jvst_cnode *top, struct jvst_ir_stmt *frame);

// Checks if an AND node requires splitting the validator.  An AND node
// will not require splitting the validator if none of its children are
// control cnodes (OR, XOR, NOT).
static bool
cnode_and_requires_split(struct jvst_cnode *and_node)
{
	struct jvst_cnode *n;

	assert(and_node->type == JVST_CNODE_AND);

	for(n=and_node->u.ctrl; n != NULL; n = n->next) {
		switch (n->type) {
		case JVST_CNODE_OR:
		case JVST_CNODE_XOR:
		case JVST_CNODE_NOT:
			return true;

		case JVST_CNODE_AND:
			fprintf(stderr,
				"canonical cnode tree should not "
				"have ANDs as children of ANDs\n");
			abort();

		case JVST_CNODE_INVALID:
		case JVST_CNODE_VALID:
		case JVST_CNODE_SWITCH:
		case JVST_CNODE_COUNT_RANGE:
		case JVST_CNODE_STR_MATCH:
		case JVST_CNODE_NUM_RANGE:
		case JVST_CNODE_NUM_INTEGER:
		case JVST_CNODE_OBJ_PROP_SET:
		case JVST_CNODE_OBJ_PROP_MATCH:
		case JVST_CNODE_OBJ_REQUIRED:
		case JVST_CNODE_ARR_ITEM:
		case JVST_CNODE_ARR_ADDITIONAL:
		case JVST_CNODE_ARR_UNIQUE:
		case JVST_CNODE_OBJ_REQMASK:
		case JVST_CNODE_OBJ_REQBIT:
		case JVST_CNODE_MATCH_SWITCH:
		case JVST_CNODE_MATCH_CASE:
			/* nop */
			continue;
		}

		// fail if the node type isn't handled in the switch
		fprintf(stderr, "unknown cnode type %d\n", and_node->type);
		abort();
	}

	return false;
}

// Counts the number of split frames required and the number of control
// nodes that split the validator.  This is to allow us to optimize
// evaluation when there's 0-1 control nodes that split the validator.
static size_t
cnode_count_splits(struct jvst_cnode *top, size_t *np)
{
	struct jvst_cnode *node;
	size_t nsplit = 0, ncontrol = 0;

	// descend from top node, count number of splits and number of
	// control nodes.
	//
	// note that we use 'goto finish' instead of break.  This is
	// to let uncaught cases fall through the loop instead of using
	// a default case.  This allows the compiler to yell at us if we
	// add another cnode type and don't add a case here.
	switch (top->type) {
	case JVST_CNODE_AND:
		if (!cnode_and_requires_split(top)) {
			goto finish;
		}

		/* fallthrough */

	case JVST_CNODE_OR:
	case JVST_CNODE_XOR:
		ncontrol++;

		// all nodes require a split
		for (node=top->u.ctrl; node != NULL; node = node->next) {
			size_t nctrl;
			nsplit++;
			nctrl = 0;
			nsplit += cnode_count_splits(node, &nctrl);
			ncontrol += nctrl;
		}
		goto finish;

	case JVST_CNODE_NOT:
		// child requires a split
		ncontrol++;
		nsplit++;
		goto finish;

	case JVST_CNODE_INVALID:
	case JVST_CNODE_VALID:
	case JVST_CNODE_SWITCH:
	case JVST_CNODE_COUNT_RANGE:
	case JVST_CNODE_STR_MATCH:
	case JVST_CNODE_NUM_RANGE:
	case JVST_CNODE_NUM_INTEGER:
	case JVST_CNODE_OBJ_PROP_SET:
	case JVST_CNODE_OBJ_PROP_MATCH:
	case JVST_CNODE_OBJ_REQUIRED:
	case JVST_CNODE_ARR_ITEM:
	case JVST_CNODE_ARR_ADDITIONAL:
	case JVST_CNODE_ARR_UNIQUE:
	case JVST_CNODE_OBJ_REQMASK:
	case JVST_CNODE_OBJ_REQBIT:
	case JVST_CNODE_MATCH_SWITCH:
	case JVST_CNODE_MATCH_CASE:
		/* nop */
		goto finish;
	}

	// fail if the node type isn't handled in the switch
	fprintf(stderr, "unknown cnode type %d\n", top->type);
	abort();

finish:
	*np = ncontrol;
	return nsplit;
}

struct split_gather_data {
	struct jvst_ir_stmt **fpp;
	struct jvst_ir_stmt *bvec;
	size_t boff;
	size_t nframe; // number of frames
	size_t nctrl;  // number of control nodes
};

// Separates children of a control node into two separate linked lists:
// one control and one non-control. Returns opp when finished so the
// linked lists can be easily merged
struct jvst_cnode **
separate_control_nodes(struct jvst_cnode *top, struct jvst_cnode **cpp, struct jvst_cnode **opp)
{
	struct jvst_cnode *node, *next;
	for (node=top->u.ctrl; node != NULL; node = next) {
		next = node->next;

		switch (node->type) {
		case JVST_CNODE_AND:
		case JVST_CNODE_OR:
		case JVST_CNODE_XOR:
		case JVST_CNODE_NOT:
			if (node->type == top->type) {
				fprintf(stderr, "%s:%d canonical cnode tree should not "
						"have %s nodes as children of %s nodes\n",
					__FILE__, __LINE__,
					jvst_cnode_type_name(node->type),
					jvst_cnode_type_name(top->type));
				abort();
			}

			*cpp = node;
			cpp = &node->next;
			*cpp = NULL;
			continue;

		case JVST_CNODE_INVALID:
		case JVST_CNODE_VALID:
		case JVST_CNODE_SWITCH:
		case JVST_CNODE_COUNT_RANGE:
		case JVST_CNODE_STR_MATCH:
		case JVST_CNODE_NUM_RANGE:
		case JVST_CNODE_NUM_INTEGER:
		case JVST_CNODE_OBJ_PROP_SET:
		case JVST_CNODE_OBJ_PROP_MATCH:
		case JVST_CNODE_OBJ_REQUIRED:
		case JVST_CNODE_ARR_ITEM:
		case JVST_CNODE_ARR_ADDITIONAL:
		case JVST_CNODE_ARR_UNIQUE:
		case JVST_CNODE_OBJ_REQMASK:
		case JVST_CNODE_OBJ_REQBIT:
		case JVST_CNODE_MATCH_SWITCH:
		case JVST_CNODE_MATCH_CASE:
			*opp = node;
			opp = &node->next;
			*opp = NULL;
			continue;
		}

		fprintf(stderr, "[%s:%d] unknown cnode type %d\n",
				__FILE__, __LINE__, top->type);
		abort();
	}

	return opp;
}

static struct jvst_ir_expr *
split_gather(struct jvst_cnode *top, struct split_gather_data *data)
{
	struct jvst_cnode *node;
	size_t nf;

	// Gathers split information:
	//
	// 1. All frames required by split (and convenient count)
	// 2. Expression to evaluate split (in the general case)
	// 3. Number of splits

	switch (top->type) {
	case JVST_CNODE_AND:
		{
			struct jvst_cnode *next, *ctrl, *other, **opp;
			struct jvst_ir_expr *expr, **epp;
			size_t b0;

			// AND subnodes that are not control nodes (OR, XOR,
			// NOT) can be evaluated simultaneously in the same
			// validator and do not require a split.
			//
			// 1. Separate nodes into control and non-control nodes.
			ctrl = NULL;
			other = NULL;
			opp = separate_control_nodes(top, &ctrl, &other);

			expr = NULL;
			epp = &expr;

			top->u.ctrl = other;

			// 2. Create a single frame for all non-control nodes.
			//    - Create a temporary AND junction for the IR frame
			//    - Create the IR frame
			if (other != NULL) {
				struct jvst_ir_stmt *fr;
				struct jvst_ir_expr *e_and, *e_btest;

				b0 = data->boff++;
				data->nframe++;

				fr = ir_stmt_frame();
				fr->u.frame.stmts = ir_translate_object_inner(top, fr);
				*data->fpp = fr;
				data->fpp = &fr->next;

				e_btest = ir_expr_new(JVST_IR_EXPR_BTEST);
				e_btest->u.btest.frame = data->bvec->u.bitvec.frame;
				e_btest->u.btest.label = data->bvec->u.bitvec.label;
				e_btest->u.btest.ind   = data->bvec->u.bitvec.ind;
				e_btest->u.btest.b0 = b0;
				e_btest->u.btest.b1 = b0;

				if (ctrl != NULL) {
					e_and = ir_expr_new(JVST_IR_EXPR_AND);
					e_and->u.and_or.left = e_btest;
					*epp = e_and;
					epp = &e_and->u.and_or.right;
				} else {
					*epp = e_btest;
				}
			}

			// don't lose children of AND node
			*opp = ctrl;

			if (ctrl != NULL) {
				data->nctrl++;
			}

			// 3. Create separate frames for all control nodes.
			for (node = ctrl; node != NULL; node = node->next) {
				struct jvst_ir_expr *e_ctrl;
				e_ctrl = split_gather(node, data);
				if (node->next == NULL) {
					*epp = e_ctrl;
				} else {
					struct jvst_ir_expr *e_and;
					e_and = ir_expr_new(JVST_IR_EXPR_AND);
					e_and->u.and_or.left = e_ctrl;
					*epp = e_and;
					epp = &e_and->u.and_or.right;
				}
			}
			assert(expr != NULL);
			return expr;
		}

	case JVST_CNODE_OR:
		{
			struct jvst_cnode *next, *ctrl, *other, **opp;
			struct jvst_ir_expr *expr, **epp;

			// OR and XOR subnodes must all be evaluated
			// separately, but we can use BTESTANY to
			// evaluate children that are not control nodes.

			data->nctrl++;

			// 1. Separate nodes into control and non-control nodes.
			ctrl = NULL;
			other = NULL;
			opp = separate_control_nodes(top, &ctrl, &other);

			expr = NULL;
			epp = &expr;

			top->u.ctrl = other;

			if (other != NULL) {
				struct jvst_ir_expr *e_btest;
				size_t b0;

				b0 = data->boff;

				// 2. Create frames for all non-control nodes.
				for (node = other; node != NULL; node = node->next) {
					struct jvst_ir_stmt *fr;

					data->boff++;
					data->nframe++;

					fr = ir_stmt_frame();
					fr->u.frame.stmts = ir_translate_object_inner(node, fr);
					*data->fpp = fr;
					data->fpp = &fr->next;

				}

				e_btest = ir_expr_new(JVST_IR_EXPR_BTESTANY);
				e_btest->u.btest.frame = data->bvec->u.bitvec.frame;
				e_btest->u.btest.label = data->bvec->u.bitvec.label;
				e_btest->u.btest.ind   = data->bvec->u.bitvec.ind;
				e_btest->u.btest.b0 = b0;
				e_btest->u.btest.b1 = data->boff-1;

				if (ctrl != NULL) {
					struct jvst_ir_expr *e_or;
					e_or = ir_expr_new(JVST_IR_EXPR_OR);
					e_or->u.and_or.left = e_btest;
					*epp = e_or;
					epp = &e_or->u.and_or.right;
				} else {
					*epp = e_btest;
				}
			}

			// don't lose children of OR/XOR node
			*opp = ctrl;

			// 3. Create separate frames for all control nodes.
			for (node = ctrl; node != NULL; node = node->next) {
				struct jvst_ir_expr *e_ctrl;
				e_ctrl = split_gather(node, data);
				if (node->next == NULL) {
					*epp = e_ctrl;
				} else {
					struct jvst_ir_expr *e_or;
					e_or = ir_expr_new(JVST_IR_EXPR_OR);
					e_or->u.and_or.left = e_ctrl;
					*epp = e_or;
					epp = &e_or->u.and_or.right;
				}
			}

			assert(expr != NULL);
			return expr;
		}

	case JVST_CNODE_XOR:
	case JVST_CNODE_NOT:
		// fail if the node type isn't handled in the switch
		fprintf(stderr, "%s:%d cnode %s not yet implemented in %s\n",
			__FILE__, __LINE__, jvst_cnode_type_name(top->type), __func__);
		abort();

	case JVST_CNODE_INVALID:
	case JVST_CNODE_VALID:
	case JVST_CNODE_SWITCH:
	case JVST_CNODE_COUNT_RANGE:
	case JVST_CNODE_STR_MATCH:
	case JVST_CNODE_NUM_RANGE:
	case JVST_CNODE_NUM_INTEGER:
	case JVST_CNODE_OBJ_PROP_SET:
	case JVST_CNODE_OBJ_PROP_MATCH:
	case JVST_CNODE_OBJ_REQUIRED:
	case JVST_CNODE_ARR_ITEM:
	case JVST_CNODE_ARR_ADDITIONAL:
	case JVST_CNODE_ARR_UNIQUE:
	case JVST_CNODE_OBJ_REQMASK:
	case JVST_CNODE_OBJ_REQBIT:
	case JVST_CNODE_MATCH_SWITCH:
	case JVST_CNODE_MATCH_CASE:
		// fail if the node type isn't handled in the switch
		fprintf(stderr, "%s:%d cnode %s not supported in %s\n",
			__FILE__, __LINE__, jvst_cnode_type_name(top->type), __func__);
		abort();
	}

	// fail if the node type isn't handled in the switch
	fprintf(stderr, "unknown cnode type %d\n", top->type);
	abort();
}

static void
gather_remove_bvec(struct jvst_ir_stmt *frame, struct split_gather_data *gather)
{
	struct jvst_ir_stmt **bvpp;

	// remove bitvec, but keep counter the same so any
	// bitvectors added will still have unique names...
	for(bvpp = &frame->u.frame.bitvecs; *bvpp != NULL; bvpp = &(*bvpp)->next) {
		if (*bvpp == gather->bvec) {
			*bvpp = (*bvpp)->next;
			return;
		}
	}
}

static struct jvst_ir_stmt *
ir_translate_object_split(struct jvst_cnode *top, struct jvst_ir_stmt *frame)
{
	struct jvst_cnode *node;
	struct jvst_ir_stmt *frames, *cond, **spp;
	struct jvst_ir_expr *split;

	struct split_gather_data gather = { NULL };

	frames = NULL;
	gather.fpp = &frames;
	gather.bvec = ir_stmt_bitvec(frame, "splitvec", 0);

	split = split_gather(top, &gather);
	assert(frames != NULL);

	if (gather.nctrl == 0) {
		assert(gather.nframe == 1);
		assert(frames->next == NULL);

		gather_remove_bvec(frame, &gather);

		// retranslate to remove the extra frame
		return ir_translate_object_inner(top, frame);
	}

	if (gather.nctrl == 1) {
		struct jvst_ir_expr *cmp;

		gather_remove_bvec(frame, &gather);

		// replace bit logic with simpler SPLIT logic
		split = ir_expr_new(JVST_IR_EXPR_SPLIT);
		split->u.split.frames = frames;

		cmp = NULL;
		switch (top->type) {
		case JVST_CNODE_AND:
			fprintf(stderr, "%s:%d AND cnode should not have nctrl == 1 (either 0 or 2+) in %s\n",
					__FILE__, __LINE__, __func__);
			abort();

		case JVST_CNODE_OR:
			cmp = ir_expr_op(JVST_IR_EXPR_GE, split, ir_expr_size(1));
			break;

		case JVST_CNODE_NOT:
			cmp = ir_expr_op(JVST_IR_EXPR_EQ, split, ir_expr_size(0));
			break;

		case JVST_CNODE_XOR:
			cmp = ir_expr_op(JVST_IR_EXPR_EQ, split, ir_expr_size(1));
			break;

		default:
			fprintf(stderr, "%s:%d invalid cnode type for %s: %s\n",
					__FILE__, __LINE__, __func__, jvst_cnode_type_name(top->type));
			abort();
		}

		cond = ir_stmt_if(cmp,
			ir_stmt_new(JVST_IR_STMT_VALID),
			ir_stmt_invalid(JVST_INVALID_SPLIT_CONDITION));  // XXX - improve error message!

		return cond;
	}

	// needs full bitvec logic
	cond = ir_stmt_new(JVST_IR_STMT_SEQ);
	spp = &cond->u.stmt_list;

	gather.bvec->u.bitvec.nbits = gather.nframe;
	assert(gather.boff == gather.nframe);

	*spp = ir_stmt_new(JVST_IR_STMT_SPLITVEC);
	(*spp)->u.splitvec.split_frames = frames;

	(*spp)->u.splitvec.frame = frame;
	(*spp)->u.splitvec.bitvec = gather.bvec;
	(*spp)->u.splitvec.label = gather.bvec->u.bitvec.label;
	(*spp)->u.splitvec.ind   = gather.bvec->u.bitvec.ind;

	spp = &(*spp)->next;
	*spp = ir_stmt_new(JVST_IR_STMT_IF);
	*spp = ir_stmt_if(split,
		ir_stmt_new(JVST_IR_STMT_VALID),
		ir_stmt_invalid(JVST_INVALID_SPLIT_CONDITION));  // XXX - improve error message!

	return cond;
}

static struct jvst_ir_stmt *
ir_translate_object_inner(struct jvst_cnode *top, struct jvst_ir_stmt *frame)
{
	struct jvst_ir_stmt *stmt, *pseq, **spp, **pseqpp;
	struct jvst_cnode *pmatch;
	struct fsm_options *opts;
	const char *loopname;
	size_t nreqs;
	
	struct ir_object_builder builder = { 0 };

	builder.frame = frame;

	stmt = ir_stmt_new(JVST_IR_STMT_SEQ);
	spp = &stmt->u.stmt_list;
	builder.pre_loop = spp;

	builder.oloop = ir_stmt_loop(frame,"L_OBJ");
	*spp = builder.oloop;
	builder.post_loop = &builder.oloop->next;

	spp = &(*spp)->u.loop.stmts;

	*spp = ir_stmt_new(JVST_IR_STMT_TOKEN);
	spp = &(*spp)->next;

	pseq = ir_stmt_new(JVST_IR_STMT_SEQ);
	*spp = ir_stmt_if(
		ir_expr_istok(SJP_OBJECT_END),
		ir_stmt_break(builder.oloop),
		pseq);

	builder.pre_match = &pseq->u.stmt_list;
	pseqpp = &pseq->u.stmt_list;

	builder.match = ir_stmt_new(JVST_IR_STMT_MATCH);
	builder.mcpp = &builder.match->u.match.cases;

	*pseqpp = builder.match;
	builder.match->u.match.default_case = obj_default_case();
	pseqpp = &(*pseqpp)->next;
	builder.post_match = pseqpp;
	assert(pseqpp != NULL);
	assert(*pseqpp == NULL);

	builder.matcher = NULL;

	ir_translate_obj_inner(top, &builder);

	if (builder.match->u.match.default_case == NULL) {
		builder.match->u.match.default_case = obj_default_case();
	}

	// handle post-loop constraints
	if (*builder.post_loop == NULL) {
		*builder.post_loop = ir_stmt_new(JVST_IR_STMT_VALID);
	}

	return stmt;
}

static struct jvst_ir_stmt *
ir_translate_object(struct jvst_cnode *top, struct jvst_ir_stmt *frame)
{
	switch (top->type) {
	case JVST_CNODE_AND:
	case JVST_CNODE_OR:
	case JVST_CNODE_NOT:
	case JVST_CNODE_XOR:
		return ir_translate_object_split(top, frame);

	default:
		return ir_translate_object_inner(top,frame);
	}
}

static struct jvst_ir_stmt *
ir_translate_type(enum SJP_EVENT type, struct jvst_cnode *top, struct jvst_ir_stmt *frame)
{
	switch (type) {
	case SJP_NUMBER:
		return ir_translate_number(top);

	case SJP_OBJECT_BEG:
		return ir_translate_object(top, frame);

	case SJP_NONE:
	case SJP_NULL:
	case SJP_TRUE:
	case SJP_FALSE:
	case SJP_STRING:
	case SJP_ARRAY_BEG:
		return ir_stmt_new(JVST_IR_STMT_NOP);

	case SJP_OBJECT_END:
	case SJP_ARRAY_END:
		fprintf(stderr, "%s:%d Invalid event type %d\n",
			__FILE__, __LINE__, type);
		abort();

	default:
		fprintf(stderr, "%s:%d Unknown event type %d\n",
			__FILE__, __LINE__, type);
		abort();
	}
}

struct jvst_ir_stmt *
jvst_ir_translate(struct jvst_cnode *ctree)
{
	struct jvst_ir_stmt *frame, **spp;
	int count_valid, count_invalid, count_other;
	enum jvst_cnode_type dft_case;
	size_t i;

	if (ctree->type != JVST_CNODE_SWITCH) {
		fprintf(stderr, "%s:%d translation must start at a SWITCH node\n",
			__FILE__, __LINE__);
		abort();
	}

	frame = ir_stmt_frame();
	spp = &frame->u.frame.stmts;

	// 1) Emit TOKEN
	*spp = ir_stmt_new(JVST_IR_STMT_TOKEN);
	spp = &(*spp)->next;

	// 2) count clauses that are VALID / INVALID / neither
	count_valid = 0;
	count_invalid = 0;
	count_other = 0;
	for (i=0; i < ARRAYLEN(ctree->u.sw); i++) {
		switch (ctree->u.sw[i]->type) {
		case JVST_CNODE_INVALID:
			count_invalid++;
			break;

		case JVST_CNODE_VALID:
			count_valid++;
			break;

		default:
			count_other++;
			break;
		}
	}

	// 3) pick default case (VALID/INVALID)
	// at least two cases should always be INVALID (OBJECT_END,
	// ARRAY_END)
	dft_case = (count_valid > count_invalid) ? JVST_CNODE_VALID : JVST_CNODE_INVALID;

	// 4) write IF tree, descending for each type
	for (i=0; i < ARRAYLEN(ctree->u.sw); i++) {
		struct jvst_ir_stmt *stmt, *br_true;

		if (ctree->u.sw[i]->type == dft_case) {
			continue;
		}

		switch (ctree->u.sw[i]->type) {
		case JVST_CNODE_INVALID:
			br_true = ir_stmt_invalid(JVST_INVALID_UNEXPECTED_TOKEN);
			break;

		case JVST_CNODE_VALID:
			br_true = ir_stmt_valid();
			break;

		default:
			br_true = ir_translate_type(i, ctree->u.sw[i], frame);
			break;
		}

		*spp = ir_stmt_if(ir_expr_istok(i), br_true, NULL);
		spp = &(*spp)->u.if_.br_false;
	}

	*spp = (dft_case == JVST_CNODE_VALID)
		? ir_stmt_new(JVST_IR_STMT_VALID) 
		: ir_stmt_invalid(JVST_INVALID_UNEXPECTED_TOKEN)
		;

	return frame;
}

void
jvst_ir_print(struct jvst_ir_stmt *stmt)
{
	size_t i;
	// FIXME: gross hack
	char buf[65536] = {0}; // 64K

	jvst_ir_dump(stmt, buf, sizeof buf);
	for (i=0; i < 72; i++) {
		fprintf(stderr, "-");
	}
	fprintf(stderr, "\n");
	fprintf(stderr, "%s\n", buf);
}

/* vim: set tabstop=8 shiftwidth=8 noexpandtab: */
