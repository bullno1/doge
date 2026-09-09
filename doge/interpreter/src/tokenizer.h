#ifndef DOGE_TOKENIZER_H
#define DOGE_TOKENIZER_H

#include <doge.h>
#include <bscn.h>

typedef enum {
	DOGE_TOKEN_EOF,
	DOGE_TOKEN_EOL,
	DOGE_TOKEN_WORD,
	DOGE_TOKEN_NUMBER,
	DOGE_TOKEN_STRING,
} doge_token_type_t;

typedef struct {
} doge_token_t;

typedef struct {
	bscn_t bscn;
} doge_tokenizer_t;

void
doge_tokenizer_init(doge_tokenizer_t* tokenizer, doge_str_t str);

#endif
