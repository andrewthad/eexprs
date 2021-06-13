#include <assert.h>

#include "parser/util.h"
#include "shim/common.h"


//////////////////////////////////// Helper Procedures ////////////////////////////////////

void mkUnbalanceError(lexer* st) {
  if (st->fatal.type != EEXPRERR_NOERROR) { return; }
  token* lookahead = parser_peek(st);
  st->fatal.type = EEXPRERR_UNBALANCED_WRAP;
  st->fatal.loc = lookahead->loc;
  if (st->wrapStack.len == 0) {
    st->fatal.as.unbalancedWrap.type = UCHAR_NULL;
  }
  else {
    const openWrap* match = dynarr_peek_openWrap(&st->wrapStack);
    st->fatal.as.unbalancedWrap.type = match->type;
    st->fatal.as.unbalancedWrap.loc = match->loc;
  }
}


//////////////////////////////////// Individual Expression Parsers ////////////////////////////////////

eexpr* parseSemicolon(parser* st);
eexpr* parseSpace(parser* st);

/*
```
wrapExpr
  ::= '(' semicolonExpr? ')'
   |  '[' semicolonExpr? ']'
   |  '{' semicolonExpr? '}'
   |  indent semicolonExpr (newline semicolonExpr)* dedent
```
*/
eexpr* parseWrap(parser* st) {
  token* open = parser_peek(st);
  if ( open->type != TOK_WRAP
    || !open->as.wrap.isOpen
     ) { return NULL; }
  eexpr* out = malloc(sizeof(eexpr));
  checkOom(out);
  {
    openWrap openInfo = {.loc = open->loc, .type = open->as.wrap.type};
    switch (open->as.wrap.type) {
      case WRAP_NULL: assert(false);
      case WRAP_PAREN: {
        dynarr_push_openWrap(&st->wrapStack, &openInfo);
        out->type = EEXPR_PAREN;
        goto nonIndent;
      }; break;
      case WRAP_BRACK: {
        dynarr_push_openWrap(&st->wrapStack, &openInfo);
        out->type = EEXPR_BRACK;
        goto nonIndent;
      }; break;
      case WRAP_BRACE: {
        dynarr_push_openWrap(&st->wrapStack, &openInfo);
        out->type = EEXPR_BRACE;
        goto nonIndent;
      }; break;
      case WRAP_BLOCK: {
        dynarr_push_openWrap(&st->wrapStack, &openInfo);
        out->type = EEXPR_BLOCK;
        goto indent;
      }; break;
    }
  } assert(false);
  nonIndent: {
    out->loc.start = open->loc.start;
    parser_pop(st);
    out->as.wrap = parseSemicolon(st);
    token* close = parser_peek(st);
    if ( st->wrapStack.len != 0
      && close->type == TOK_WRAP
      && !close->as.wrap.isOpen
      && close->as.wrap.type == dynarr_peek_openWrap(&st->wrapStack)->type
       ) {
      dynarr_pop_openWrap(&st->wrapStack);
      out->loc.end = close->loc.end;
      parser_pop(st);
    }
    else {
      mkUnbalanceError(st);
    }
    return out;
  } assert(false);
  indent: {
    out->loc.start = open->loc.start;
    parser_pop(st);
    dynarr_init_eexpr_p(&out->as.list, 4);
    while (true) {
      eexpr* subexpr = parseSemicolon(st);
      if (subexpr != NULL) {
        dynarr_push_eexpr_p(&out->as.list, &subexpr);
      }
      token* lookahead = parser_peek(st);
      if (lookahead->type == TOK_WRAP) {
        if ( st->wrapStack.len != 0
          && !lookahead->as.wrap.isOpen
          && lookahead->as.wrap.type == dynarr_peek_openWrap(&st->wrapStack)->type
           ) {
          dynarr_pop_openWrap(&st->wrapStack);
          out->loc.end = lookahead->loc.end;
          parser_pop(st);
        }
        else {
          mkUnbalanceError(st);
        }
        return out;
      }
      else if (lookahead->type == TOK_NEWLINE) {
        parser_pop(st);
      }
      else {
        eexprError err = {.loc = lookahead->loc, .type = EEXPRERR_EXPECTING_NEWLINE_OR_DEDENT};
        dllist_insertAfter_eexprError(&st->errStream, NULL, &err);
        return out;
      }
    }
  }; assert(false);
}

/*
```
stringTemplate
  ::= string.plain
   |  string.open spaceExpr (string.middle spaceExpr)* string.close
```
*/
eexpr* parseTemplate(parser* st) {
  token* tok = parser_peek(st);
  if (tok->type != TOK_STRING) { return NULL; }
  switch (tok->as.string.splice) {
    case STRSPLICE_PLAIN: {
      eexpr* out = malloc(sizeof(eexpr));
      checkOom(out);
      out->loc = tok->loc;
      out->type = EEXPR_STRING;
      out->as.string.text1 = tok->as.string.text;
      out->as.string.parts.cap = 0;
      out->as.string.parts.len = 0;
      out->as.string.parts.data = NULL;
      parser_pop(st);
      return out;
    }; break;
    case STRSPLICE_OPEN: {
      eexpr* out = malloc(sizeof(eexpr));
      checkOom(out);
      { // initialize output buffer
        out->loc = tok->loc;
        out->type = EEXPR_STRING;
        out->as.string.text1 = tok->as.string.text;
        dynarr_init_strTemplPart(&out->as.string.parts, 2);
      }
      { // push to wrapStack
        openWrap info = {.loc = tok->loc, .type = '\"'};
        dynarr_push_openWrap(&st->wrapStack, &info);
      }
      parser_pop(st);
      while (true) {
        strTemplPart part;
        { // we need an expr before the next part of the template
          // but if the next part comes without an expr, we can do some error recovery later
          // we flag that recovery is needed by setting part.expr to NULL
          token* lookahead = parser_peek(st);
          if ( lookahead->type != TOK_STRING
            || (lookahead->as.string.splice != STRSPLICE_MIDDLE && lookahead->as.string.splice != STRSPLICE_CLOSE)
             ) {
            part.expr = parseSpace(st);
          }
          else {
            part.expr = NULL;
          }
        }
        token* lookahead = parser_peek(st);
        if (part.expr != NULL) {
          out->loc.end = part.expr->loc.end;
        }
        else {
          eexprError err = {.loc = lookahead->loc, .type = EEXPRERR_MISSING_TEMPLATE_EXPR};
          if ( lookahead->type == TOK_STRING
            && (lookahead->as.string.splice == STRSPLICE_MIDDLE || lookahead->as.string.splice == STRSPLICE_CLOSE)
             ) {
            dllist_insertAfter_eexprError(&st->errStream, NULL, &err);
          }
          else {
            st->fatal = err;
            return out;
          }
        }
        if (lookahead->type == TOK_STRING) {
          { // append last template part
            part.textAfter = lookahead->as.string.text;
            if (part.expr != NULL) {
              dynarr_push_strTemplPart(&out->as.string.parts, &part);
            }
            out->loc.end = lookahead->loc.end;
          }
          // ensure we are expecting a close string
          if (st->wrapStack.len == 0 || dynarr_peek_openWrap(&st->wrapStack)->type != '\"') {
            mkUnbalanceError(st);
          }
          else { // ensure the splice type makes sense
            if (lookahead->as.string.splice == STRSPLICE_CLOSE) {
              dynarr_pop_openWrap(&st->wrapStack);
              parser_pop(st);
              return out;
            }
            else if (lookahead->as.string.splice == STRSPLICE_MIDDLE) {
              parser_pop(st);
            }
            else { assert(false); }
          }
        }
        else {
          if (part.expr != NULL) {
            part.textAfter.len = 0; part.textAfter.bytes = NULL;
            dynarr_push_strTemplPart(&out->as.string.parts, &part);
          }
          eexprError err = {.loc = lookahead->loc, .type = EEXPRERR_MISSING_CLOSE_TEMPLATE};
          dllist_insertAfter_eexprError(&st->errStream, NULL, &err);
          return out;
        }
      }
      assert(false);
    }; break;
    default: {
      mkUnbalanceError(st);
      return NULL;
    }; break;
  }
}

/*
```
atomicExpr
  ::= symbol
   |  number
   |  codepoint
   |  stringTemplate
   |  wrapExpr
```
*/
eexpr* parseAtomic(parser* st) {
  token* tok = parser_peek(st);
  switch (tok->type) {
    case TOK_SYMBOL: {
      eexpr* out = malloc(sizeof(eexpr));
      checkOom(out);
      out->loc = tok->loc;
      out->type = EEXPR_SYMBOL;
      out->as.symbol = tok->as.symbol;
      parser_pop(st);
      return out;
    }; break;
    case TOK_NUMBER: {
      eexpr* out = malloc(sizeof(eexpr));
      checkOom(out);
      out->loc = tok->loc;
      out->type = EEXPR_NUMBER;
      out->as.number = tok->as.number;
      parser_pop(st);
      return out;
    }; break;
    case TOK_CODEPOINT: {
      eexpr* out = malloc(sizeof(eexpr));
      checkOom(out);
      out->loc = tok->loc;
      out->type = EEXPR_CODEPOINT;
      out->as.codepoint = tok->as.codepoint;
      parser_pop(st);
      return out;
    }; break;
    case TOK_STRING: {
      return parseTemplate(st);
    }; break;
    case TOK_WRAP: {
      return parseWrap(st);
    }; break;
    default: {
      return NULL;
    }
  }
}

/*
```
chainExpr ::= atomicExpr chainTail*
chainTail
  ::= chainDot atomicExpr
   |  wrapExpr
   |  stringTemplate
```
Note that `stringTemplate strTemplPart` is not a chainExpr, since the postlexer should already have detected it as crammed tokens.
*/
eexpr* parseChain(parser* st) {
  eexpr* predot = NULL;
  { // check if this is a predot expression
    token* lookahead = parser_peek(st);
    if (lookahead->type == TOK_PREDOT) {
      predot = malloc(sizeof(eexpr));
      checkOom(predot);
      predot->type = EEXPR_PREDOT;
      predot->loc.start = lookahead->loc.start;
      parser_pop(st);
    }
  }
  eexpr* chain = NULL;
  {
    { // get the first expression and look for a following dot
      eexpr* expr1 = parseAtomic(st);
      if (expr1 == NULL) {
        goto finish;
      }
      token* lookahead = parser_peek(st);
      if ( lookahead->type == TOK_CHAIN
        || (lookahead->type == TOK_WRAP && lookahead->as.wrap.isOpen)
        || ( lookahead->type == TOK_STRING
          && (lookahead->as.string.splice == STRSPLICE_PLAIN || lookahead->as.string.splice == STRSPLICE_OPEN)
           )
         ) {
        chain = malloc(sizeof(eexpr));
        checkOom(chain);
        chain->type = EEXPR_CHAIN;
        chain->loc = expr1->loc;
        if (lookahead->type == TOK_CHAIN) {
          chain->loc.end = lookahead->loc.end;
          parser_pop(st);
        }
        dynarr_init_eexpr_p(&chain->as.list, 4);
        dynarr_push_eexpr_p(&chain->as.list, &expr1);
      }
      else {
        chain = expr1;
        goto finish;
      }
    }
    while (true) { // get further chained expressions
      eexpr* next = parseAtomic(st);
      if (next == NULL) { goto finish; }
      dynarr_push_eexpr_p(&chain->as.list, &next);
      token* lookahead = parser_peek(st);
      if (lookahead->type == TOK_CHAIN) {
        // continue the chain when there's another chain dot
        chain->loc.end = lookahead->loc.end;
        parser_pop(st);
      }
      else if (lookahead->type == TOK_WRAP && lookahead->as.wrap.isOpen) {
        // continue the chain when there's an open paren/brace/brack/indent
        chain->loc.end = next->loc.end;
      }
      else if ( lookahead->type == TOK_STRING
             && (lookahead->as.string.splice == STRSPLICE_PLAIN || lookahead->as.string.splice == STRSPLICE_OPEN)
              ) {
        // continue the chain when there's the start of a string
        chain->loc.end = next->loc.end;
      }
      else {
        chain->loc.end = next->loc.end;
        goto finish;
      }
    }
  } assert(false);
  finish: {
    if (predot == NULL && chain == NULL) {
      return NULL;
    }
    else if (predot == NULL) {
      return chain;
    }
    else {
      predot->loc.end = chain->loc.end;
      predot->as.wrap = chain;
      return predot;
    }
  } assert(false);
}

/*
spaceExpr ::= chainExpr (whitespace chainExpr)*
*/
eexpr* parseSpace(parser* st) {
  if (parser_peek(st)->type == TOK_SPACE) {
    parser_pop(st);
  }
  eexpr* expr1 = parseChain(st);
  if (expr1 == NULL) { return NULL; }
  eexpr* out = malloc(sizeof(eexpr));
  { // prepare the output
    checkOom(out);
    out->loc.start = expr1->loc.start;
    out->loc.end = expr1->loc.end;
    out->type = EEXPR_SPACE;
    dynarr_init_eexpr_p(&out->as.list, 4);
    dynarr_push_eexpr_p(&out->as.list, &expr1);
  }
  while (true) {
    token* lookahead = parser_peek(st);
    if (lookahead->type == TOK_SPACE) {
      parser_pop(st);
      eexpr* next = parseChain(st);
      if (next != NULL) {
        dynarr_push_eexpr_p(&out->as.list, &next);
        out->loc.end = next->loc.end;
      }
      else {
        goto output;
      }
    }
    else{
      goto output;
    }
  } assert(false);
  output: {
    if (out->as.list.len == 1) {
      dynarr_deinit_eexpr_p(&out->as.list);
      free(out);
      return expr1;
    }
    else {
      // TODO shrink the list? and all other lists generated by the parser?
      return out;
    }
  } assert(false);
}

eexpr* parseEllipsis(parser* st) {
  eexpr* expr1 = parseSpace(st);
  token* lookahead = parser_peek(st);
  if (lookahead->type == TOK_ELLIPSIS) {
    fileloc dotsLoc = lookahead->loc;
    parser_pop(st);
    eexpr* expr2 = parseSpace(st);
    eexpr* out = malloc(sizeof(eexpr));
    checkOom(out);
    out->type = EEXPR_ELLIPSIS;
    out->loc.start = (expr1 == NULL ? dotsLoc : expr1->loc).start;
    out->loc.end = (expr2 == NULL ? dotsLoc : expr2->loc).end;
    out->as.ellipsis[0] = expr1;
    out->as.ellipsis[1] = expr2;
    return out;
  }
  else {
    return expr1;
  }
}

eexpr* parseColon(parser* st) {
  eexpr* expr1 = parseEllipsis(st);
  token* colon = parser_peek(st);
  if (colon->type != TOK_COLON) {
    return expr1;
  }
  else {
    fileloc colonLoc = colon->loc;
    parser_pop(st);
    eexpr* expr2 = parseEllipsis(st);
    if (expr2 == NULL) {
      expr1->loc.end = colonLoc.end;
      return expr1;
    }
    eexpr* out = malloc(sizeof(eexpr));
    checkOom(out);
    out->type = EEXPR_COLON;
    out->loc.start = expr1->loc.start;
    out->loc.end = expr2->loc.end;
    out->as.pair[0] = expr1;
    out->as.pair[1] = expr2;
    return out;
  }
}

eexpr* parseComma(parser* st) {
  eexpr* out = NULL;
  { // optional initial comma
    token* maybeComma = parser_peek(st);
    if (maybeComma->type == TOK_COMMA) {
      out = malloc(sizeof(eexpr));
      checkOom(out);
      dynarr_init_eexpr_p(&out->as.list, 4);
      out->loc = maybeComma->loc;
      parser_pop(st);
    }
  }
  while (true) {
    eexpr* tmp = parseColon(st);
    token* lookahead = parser_peek(st);
    if (tmp == NULL) { // no further sub-expressions
      if (out != NULL) {
        out->type = EEXPR_COMMA;
        return out;
      }
      else {
        return NULL;
      }
    }
    else if (out != NULL) { // found a sub-expression, and we already have evidence of a comma
      dynarr_push_eexpr_p(&out->as.list, &tmp);
      if (lookahead->type == TOK_COMMA) { // there's also comma afterwards to be consumed
        out->loc.end = lookahead->loc.end;
        parser_pop(st);
      }
      else {
        out->loc.end = tmp->loc.end;
      }
    }
    else if (lookahead->type == TOK_COMMA) { // found a sub-expression, and the first evidence of a comma
      out = malloc(sizeof(eexpr));
      checkOom(out);
      dynarr_init_eexpr_p(&out->as.list, 4);
      dynarr_push_eexpr_p(&out->as.list, &tmp);
      out->loc.start = tmp->loc.start;
      out->loc.end = lookahead->loc.end;
      parser_pop(st);
    }
    else { // found a sub-expression, with no evidence of a comma before, and no evidence of a comma after
      return tmp;
    }
  }
}

eexpr* parseSemicolon(parser* st) {
  eexpr* out = NULL;
  { // optional initial semicolon
    token* maybeSemi = parser_peek(st);
    if (maybeSemi->type == TOK_SEMICOLON) {
      out = malloc(sizeof(eexpr));
      checkOom(out);
      dynarr_init_eexpr_p(&out->as.list, 4);
      out->loc = maybeSemi->loc;
      parser_pop(st);
    }
  }
  while (true) {
    eexpr* tmp = parseComma(st);
    token* lookahead = parser_peek(st);
    if (tmp == NULL) { // no further sub-expressions
      if (out != NULL) {
        out->type = EEXPR_SEMICOLON;
        return out;
      }
      else {
        return NULL;
      }
    }
    else if (out != NULL) { // found a sub-expression, and we already have evidence of a semicolon
      dynarr_push_eexpr_p(&out->as.list, &tmp);
      if (lookahead->type == TOK_SEMICOLON) { // there's also semicolon afterwards to be consumed
        out->loc.end = lookahead->loc.end;
        parser_pop(st);
      }
      else {
        out->loc.end = tmp->loc.end;
      }
    }
    else if (lookahead->type == TOK_SEMICOLON) { // found a sub-expression, and the first evidence of a semicolon
      out = malloc(sizeof(eexpr));
      checkOom(out);
      dynarr_init_eexpr_p(&out->as.list, 4);
      dynarr_push_eexpr_p(&out->as.list, &tmp);
      out->loc.start = tmp->loc.start;
      out->loc.end = lookahead->loc.end;
      parser_pop(st);
    }
    else { // found a sub-expression, with no evidence of a semicolon before, and no evidence of a semicolon after
      return tmp;
    }
  }
}

//////////////////////////////////// Main Parser ////////////////////////////////////

void parseLine(parser* st) {
  eexpr* line = parseSemicolon(st);
  if (line != NULL) {
      dynarr_push_eexpr_p(&st->eexprStream, &line);
  }
  else {
    size_t depth = 0; { // count up how many dedents we currently expect, then reset the wrapStack
      for (size_t i = 0; i < st->wrapStack.len; ++i) {
        if (st->wrapStack.data[i].type == '\n') {
          depth += 1;
        }
      }
      st->wrapStack.len = 0;
    }
    // error recovery: advance through tokens until we get to the next top-level newline (or end-of-file)
    // this should work well because indents and dedents are generated already matched with each other in the postlexer
    while (true) {
      while (depth != 0) {
        token* tok = parser_peek(st);
        switch (tok->type) {
          case TOK_EOF: return;
          case TOK_WRAP: {
            if (tok->as.wrap.type == WRAP_BLOCK) {
              if (tok->as.wrap.isOpen) { depth += 1; }
              else { depth -= 1; }
            }
          }; break;
          default: /* do nothing */ break;
        }
        parser_pop(st);
      }
      while (true) {
        token* tok = parser_peek(st);
        // consume tokens until next newline/end-of-file,
        if (tok->type == TOK_NEWLINE || tok->type == TOK_EOF) {
          return;
        }
        // but if there's an indent, we'll need to go through matching depths again
        else if ( tok->type == TOK_WRAP
               && tok->as.wrap.type == WRAP_BLOCK
               && tok->as.wrap.isOpen
                ) {
          depth += 1;
          parser_pop(st);
          break;
        }
        else {
          parser_pop(st);
        }
      }
    }
  }
}

void parser_parse(parser* st) {
  bool atStart = true;
  while (st->fatal.type == EEXPRERR_NOERROR) {
    token* lookahead = parser_peek(st);
    switch (lookahead->type) {
      case TOK_NEWLINE: {
        assert(!atStart);
        parser_pop(st);
        parseLine(st);
      }; break;
      case TOK_EOF: return;
      default: {
        if (atStart) {
          atStart = false;
          parseLine(st);
        }
        else if ( lookahead->type == TOK_WRAP
               && !lookahead->as.wrap.isOpen
                ) {
          mkUnbalanceError(st);
        }
        else {
          token* tok = parser_peek(st);
          fprintf(stderr, "%zu:%zu--%zu:%zu\n", tok->loc.start.line+1, tok->loc.start.col+1, tok->loc.end.line+1, tok->loc.end.col+1);
          assert(atStart);
        }
      } break;
    }
  }
}
