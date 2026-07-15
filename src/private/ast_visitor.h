#ifndef AST_VISITOR_H
#define AST_VISITOR_H

#include "ast_types.h"
#include <setjmp.h>
#include <stdbool.h>

typedef enum {
    VISIT_CONTINUE,
    VISIT_SKIP_CHILDREN, // Process this node, but dont push children
    VISIT_ABORT,
} VisitResult;

typedef struct AstVisitor AstVisitor;

struct AstVisitor {
    void *user_data;
    jmp_buf *panic_env;

    VisitResult (*enter_node)(AstVisitor *visitor, AstNode *node);
    void (*exit_node)(AstVisitor *visitor, AstNode *node);
};

bool ast_traverse(AstVisitor *visitor, AstNode *root);

#endif // !AST_VISITOR_H
