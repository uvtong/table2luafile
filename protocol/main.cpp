
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <ctype.h>
#include <setjmp.h>
extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

#define TRY(l) if (setjmp((l)->exception) == 0)
#define THROW(l) longjmp((l)->exception, 1)

#define TYPE_INT		0
#define TYPE_NUMBER		1
#define TYPE_STRING		2
#define TYPE_PROTOCOL	3

static const char* builtin_type[] = { "int", "number", "string", "protocol" };

struct field_type {
	int type;
	struct protocol* protocol;
};

struct field {
	char* name;
	struct field_type field_type;
};

struct protocol_table;

struct protocol {
	struct protocol* next;
	struct protocol* parent;
	struct protocol_table* children;

	char* name;
	struct field** field;
	int cap;
	int size;

	char* lastfield;
};

struct protocol_table {
	struct protocol** slots;
	int size;
};

typedef struct protocol* (*protocol_begin_func)(struct protocol_table* table, const char* name);
typedef void(*protocol_over_func)(struct protocol_table* table);
typedef void(*field_begin_func)(struct protocol* ptl,const char* field_type);
typedef void(*field_over_func)(struct protocol* ptl, const char* field_name);

struct lexer {
	char* c;
	int offset;
	int size;
	int line;
	jmp_buf exception;
	struct protocol* root;
	protocol_begin_func protocol_begin;
	protocol_over_func protocol_over;
	field_begin_func field_begin;
	field_begin_func field_over;
};

size_t strhash(const char *str)
{
	size_t hash = 0;
	int ch;
	for (long i = 0; ch = (int)*str++; i++)
	{
		if ((i & 1) == 0)
			hash ^= ((hash << 7) ^ ch ^ (hash >> 3));
		else
			hash ^= (~((hash << 11) ^ ch ^ (hash >> 5)));
	}
	return hash;
}

struct protocol_table* create_table(int size)
{
	struct protocol_table* table = (struct protocol_table*)malloc(sizeof(*table));
	table->size = size;

	table->slots = (struct protocol**)malloc(sizeof(*table->slots) * size);
	memset(table->slots, 0, sizeof(*table->slots) * size);

	return table;
}

struct protocol* query_protocol(struct protocol_table* table, const char* name)
{
	size_t hash = strhash(name);
	int index = hash % table->size;
	struct protocol* slot = table->slots[index];
	if (slot != NULL)
	{
		while (slot)
		{
			if ((memcmp(slot->name, name,strlen(name)) == 0))
				return slot;
			slot = slot->next;
		}
	}
	return NULL;
}

void rehash_table(struct protocol_table* table, int nsize)
{
	struct protocol** nslots = (struct protocol**)malloc(sizeof(*nslots) * nsize);
	memset(nslots, 0, sizeof(*nslots) * nsize);

	for (int i = 0; i < table->size; i++) 
	{
		struct protocol* ptl = table->slots[i];
		while (ptl) 
		{
			int hash = strhash(ptl->name);
			int index = hash % nsize;
			struct protocol* slot = nslots[index];
			if (slot == NULL)
				nslots[index] = ptl;
			else
			{
				ptl->next = slot;
				nslots[index] = ptl;
			}
			ptl = ptl->next;
		}
	}
	free(table->slots);
	table->slots = nslots;
	table->size = nsize;
}

void add_protocol(struct protocol_table* table, struct protocol* protocol)
{
	size_t hash = strhash(protocol->name);
	int index = hash % table->size;
	struct protocol* slot = table->slots[index];
	if (slot == NULL)
		table->slots[index] = protocol;
	else
	{
		protocol->next = slot;
		table->slots[index] = protocol;
		int size = 0;
		while (protocol)
		{
			size++;
			protocol = protocol->next;
		}
		if (size >= (table->size / 8))
			rehash_table(table, table->size * 2);
	}
}

struct field* query_field(struct protocol* protocol, const char* name)
{
	for (int i = 0; i < protocol->size; i++)
	{
		struct field* f = protocol->field[i];
		if (memcmp(f->name, name, strlen(name)) == 0)
			return f;
	}
	return NULL;
}

void add_field(struct protocol* protocol, struct field* f)
{
	if (protocol->size == protocol->cap)
	{
		int ncap = protocol->cap * 2;
		struct field** nptr = (struct field**)malloc(sizeof(*nptr) * ncap);
		memset(nptr, 0, sizeof(*nptr) * ncap);
		memcpy(nptr, protocol->field, sizeof(*nptr) * protocol->cap);
		free(protocol->field);
		protocol->field = nptr;
		protocol->cap = ncap;
	}
	protocol->field[protocol->size++] = f;
}

struct field* create_field(struct protocol* ptl,char* field_type, char* field_name)
{
	int ftype = TYPE_PROTOCOL;
	for (int i = 0; i < sizeof(builtin_type) / sizeof(void*); i++)
	{
		if (memcmp(field_type, builtin_type[i], strlen(field_type)) == 0)
		{
			ftype = i;
			break;
		}
	}

	struct field* f = (struct field*)malloc(sizeof(*f));
	memset(f, 0, sizeof(*f));
	f->name = field_name;
	f->field_type.type = ftype;
	f->field_type.protocol = NULL;

	if (ftype == TYPE_PROTOCOL)
	{	
		struct protocol* cursor = ptl;
		while (cursor)
		{
			struct protocol* tmp = query_protocol(cursor->children,field_type);
			if (tmp)
			{
				f->field_type.protocol = tmp;
				break;
			}
			cursor = cursor->parent;
		}
		assert(f->field_type.protocol != NULL);
	}

	return f;
}

struct protocol* create_protocol(const char* name)
{
	int len = strlen(name);
	struct protocol* ctx = (struct protocol*)malloc(sizeof(*ctx));
	ctx->next = NULL;
	ctx->name = (char*)malloc(len + 1);
	memcpy((void*)ctx->name, (void*)name, len);
	ctx->name[len] = '\0';
	ctx->parent = NULL;
	ctx->children = create_table(4);
	ctx->cap = 4;
	ctx->size = 0;
	ctx->field = (struct field**)malloc(sizeof(struct field*) * ctx->cap);
	memset(ctx->field, 0, sizeof(struct field*) * ctx->cap);

	return ctx;
}

void lexer_init(struct lexer* parser, const char* file, void* ud, protocol_begin_func ptl_begin, protocol_over_func ptl_over, field_begin_func field_begin, field_over_func field_over)
{
	FILE *file_handle = fopen(file, "r");
	fseek(file_handle, 0, SEEK_END);
	int len = ftell(file_handle);
	parser->offset = 0;
	parser->size = len;
	parser->line = 1;
	parser->c = (char*)malloc(len+1);
	memset(parser->c, 0, len + 1);
	rewind(file_handle);
	fread(parser->c, 1, len, file_handle);
	parser->c[len] = 0;
	fclose(file_handle);

	parser->root = create_protocol("root");
	parser->protocol_begin = ptl_begin;
	parser->protocol_over = ptl_over;
	parser->field_begin = field_begin;
	parser->field_over = field_over;
}

static int eos(struct lexer *l, int n)
{
	if (*(l->c + n) == 0)
		return 1;
	else
		return 0;
}

static void skip_space(struct lexer *l);

static void next_line(struct lexer *l)
{
	char *n = l->c;
	while (*n != '\n' && *n)
		n++;
	if (*n == '\n')
		n++;
	l->line++;
	l->c = n;
	skip_space(l);

	return;
}

static void skip_space(struct lexer *l)
{
	char *n = l->c;
	while (isspace(*n) && *n) {
		if (*n == '\n')
			l->line++;
		n++;
	}

	l->c = n;
	if (*n == '#' && *n)
		next_line(l);

	return;
}

static void next_token(struct lexer *l)
{
	char *n;
	skip_space(l);
	n = l->c;
	while (!isspace(*n) && *n)
		n++;
	l->c = n;
	skip_space(l);

	return;
}

static void skip(struct lexer* l, int size)
{
	char *n = l->c;
	int index = 0;
	while (!eos(l,0) && index < size)
	{
		n++;
		index++;
	}
		
	l->c = n;
}

static bool expect(struct lexer* l, int offset, char c)
{
	return *(l->c + offset) == c;
}

static bool expect_space(struct lexer* l, int offset)
{
	return isspace(*(l->c + offset));
}

void lexer_parse(struct lexer* l, struct protocol* parent)
{
	skip_space(l);
	char name[65];
	memset(name, 0, 65);

	int err = sscanf(l->c, "%64[1-9a-zA-Z]", name);
	if (err == 0) {
		fprintf(stderr, "line:%d syntax error", l->line);
		THROW(l);
	}

	int len = strlen(name);
	if (memcmp(name, "protocol", len) != 0)
	{
		fprintf(stderr, "line:%d syntax error:expect protocol", l->line);
		THROW(l);
	}

	next_token(l);

	memset(name, 0, 65);
	err = sscanf(l->c, "%64[1-9a-zA-Z_]", name);
	struct protocol* ptl = l->protocol_begin(parent->children, name);
	ptl->parent = parent;

	len = strlen(name);

	//协议名不能超过64
	if (!expect_space(l, len) && !expect(l, len, '{'))
	{
		fprintf(stderr, "line:%d syntax error:protocol name:%s too long", l->line,name);
		THROW(l);
	}
	//跳过协议名
	skip(l, len);

	//跳过空格
	skip_space(l);

	//协议名后有>=0个空格，空格之后必须是{
	if (!expect(l, 0, '{'))
	{
		fprintf(stderr, "line:%d syntax error", l->line);
		THROW(l);
	}

	//{之后必有空格
	if (!expect_space(l, 1))
	{
		fprintf(stderr, "line:%d syntax error:expect space", l->line);
		THROW(l);
	}
	while (!eos(l,0))
	{
		next_token(l);

	__again:
		if (expect(l, 0, '}'))
		{
			if (!eos(l,1) && !expect_space(l, 1))
			{
				fprintf(stderr, "line:%d syntax error", l->line);
				THROW(l);
			}
			l->protocol_over(parent->children);
			next_token(l);
			break;
		}
		memset(name, 0, 65);
		err = sscanf(l->c, "%64s", name);
		if (err == 0)
		{
			fprintf(stderr, "line:%d syntax error", l->line);
			THROW(l);
		}
		int len = strlen(name);
		bool success = false;
		for (int i = 0; i < sizeof(builtin_type) / sizeof(void*); i++)
		{
			if (memcmp(name, builtin_type[i], len) == 0)
			{
				success = true;
				break;
			}
		}
		if (!success)
		{
			struct protocol* protocol = query_protocol(parent->children, name);
			if (protocol == NULL)
			{
				fprintf(stderr, "line:%d syntax error:unknown type:%s\n", l->line, name);
				THROW(l);
			}			
		}

		if (expect_space(l,len) == 0)
		{
			fprintf(stderr, "line:%d syntax error", l->line);
			THROW(l);
		}

		if (memcmp(name, "protocol", len) == 0)
		{
			lexer_parse(l, ptl);
			if (expect_space(l, 0))
				continue;
			else
				goto __again;
		}
		else
		{
			l->field_begin(ptl, name);
		}

		next_token(l);
		memset(name, 0, 65);
		err = sscanf(l->c, "%64[1-9a-zA-Z_]", name);
		if (err == 0)
		{
			fprintf(stderr, "line:%d syntax error", l->line);
			THROW(l);
		}
		len = strlen(name);
		l->field_over(ptl, name);
		
		//每一个字段名之后，必有空格
		if (!expect_space(l, len))
		{
			fprintf(stderr, "line:%d syntax error:expect space", l->line);
			THROW(l);
		}
	}
	
}

struct protocol* protobol_begin(struct protocol_table* table, const char* name)
{
	printf("protobol_begin:%s\n", name);
	struct protocol* ptl = create_protocol(name);
	add_protocol(table, ptl);
	return ptl;
}

void protobol_over(struct protocol_table* table)
{
	printf("protobol_over\n");
}

void field_begin(struct protocol* ptl, const char* field_type)
{
	printf("field_begin:%s\n", field_type);
	int len = strlen(field_type);
	ptl->lastfield = (char*)malloc(len+1);
	memcpy(ptl->lastfield, field_type, len);
	ptl->lastfield[len] = '\0';
}

void field_over(struct protocol* ptl, const char* field_name)
{
	printf("field_over:%s\n", field_name);
	int len = strlen(field_name);
	char* fname = (char*)malloc(len + 1);
	memcpy(fname, field_name, len);
	fname[len] = '\0';

	struct field* f = create_field(ptl,ptl->lastfield, fname);
	add_field(ptl, f);

	ptl->lastfield = NULL;
}


int main()
{
	struct lexer l;
	lexer_init(&l, "test.protocol", NULL, protobol_begin, protobol_over, field_begin, field_over);
	TRY(&l) {
		while (!eos(&l,0))
		{
			lexer_parse(&l, l.root);
		}
		return 0;
	}
}