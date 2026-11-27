/* Native Linux TL/1 compiler, first subset of the MB-6880 TL1.mot port.
 * Usage: cc -std=c11 -O2 -o TL1 main.c; ./TL1 < program.tl1 > program.s
 * Assemble with Fuzix as6800, then link with ld6800 -b -C 0x2222.
 * The generated section starts at $2222 and requires the original runtime.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { ORIGIN = 0x2222, STOP = 0x209e, PRINT_NUMBER = 0x20a1,
       PRINT_WITH_WIDTH = 0x20a2,
       PRINT_STRING = 0x2108, OUTPUT_DEVICE = 0x81,
       PRINT_CHAR = 0x2122, PRINT_SPACES = 0x20fd,
       PRINT_NEWLINE = 0x2111, PRINT_NEWLINES = 0x2119,
       RANDOM = 0x202f, GET_CHAR = 0x212e, READ_NUMBER = 0x20d8, INPUT_DEVICE = 0x80,
       MHIGH_ADDR = 0x86, MOD_ADDR = 0x87 };
enum Token { EOF_TOKEN = -1, NUMBER = 256, VARIABLE, UNKNOWN, VAR,
             BEGIN, END, WRITE, IF, THEN, WHILE, DO, REPEAT, UNTIL, STOP_WORD,
             LT, GT, AND, OR, EOR, ADC, SBC, NEG, COM, LSR, ROR, ASR, ASL, ROL,
             MHIGH, MOD, RND, GET, READ, ASCII, SPACE, CRLF, FOR, TO,
             CASE, OF, ELSE, ARRAY, ARRAY_NAME };

struct Name {
    size_t start, length;
    uint8_t offset;
    bool array;
};

static unsigned char source[65536];
static uint8_t code[65536 - ORIGIN];
static struct Name variables[65536];
static size_t source_size, cursor, token_start, token_length, code_size, variable_count;
static int token;
static uint8_t value;
static bool x_current = true;

static void fail(const char *message)
{
    size_t line = 1;
    for (size_t i = 0; i < token_start; ++i)
        if (source[i] == 5)
            ++line;
    fprintf(stderr, "TL1: line %zu: %s\n", line, message);
    exit(EXIT_FAILURE);
}

static int peek(void)
{
    return cursor < source_size ? source[cursor] : EOF_TOKEN;
}

static bool letter(int c) { return c >= 'A' && c <= 'Z'; }
static bool digit(int c) { return c >= '0' && c <= '9'; }
static bool space(int c) { return (c >= 0 && c <= ' ') || c == '.' || c == ';'; }

/* $13FB: the declaration path reads the word without resolving it. */
static void next(bool declaring)
{
    static const struct { const char *word; int token; } keywords[] = {
        {"VAR", VAR}, {"BEGIN", BEGIN}, {"END", END}, {"WRITE", WRITE},
        {"ARRAY", ARRAY},
        {"IF", IF}, {"THEN", THEN}, {"WHILE", WHILE}, {"DO", DO},
        {"REPEAT", REPEAT}, {"UNTIL", UNTIL}, {"STOP", STOP_WORD},
        {"FOR", FOR}, {"TO", TO},
        {"CASE", CASE}, {"OF", OF}, {"ELSE", ELSE},
        {"LT", LT}, {"GT", GT},
        {"AND", AND}, {"OR", OR}, {"EOR", EOR},
        {"ADC", ADC}, {"SBC", SBC},
        {"NEG", NEG}, {"NOT", COM}, {"COM", COM},
        {"LSR", LSR}, {"ROR", ROR}, {"ASR", ASR}, {"ASL", ASL}, {"ROL", ROL},
        {"MHIGH", MHIGH}, {"MOD", MOD},
        {"RND", RND}, {"GET", GET}, {"READ", READ},
        {"ASCII", ASCII}, {"SPACE", SPACE}, {"CRLF", CRLF},

        {"TRUE", NUMBER}, {"FALSE", NUMBER}
    };
    do {
        while (space(peek()))
            ++cursor;
        if (peek() != '%')
            break;
        do { ++cursor; } while (peek() >= ' ');
    } while (true);
    token_start = cursor;
    token = peek();
    if (token == EOF_TOKEN)
        return;
    ++cursor;
    if (letter(token)) {
        while (letter(peek()) || digit(peek()))
            ++cursor;
        token_length = cursor - token_start;
        token = UNKNOWN;
        if (declaring)
            return;
        for (size_t n = variable_count; n > 0; --n) {
            struct Name name = variables[n - 1];
            if (name.length == token_length &&
                !memcmp(source + name.start, source + token_start, token_length)) {
                token = name.array ? ARRAY_NAME : VARIABLE;
                value = name.offset;
                return;
            }
        }
        for (size_t n = 0; n < sizeof keywords / sizeof *keywords; ++n) {
            const char *word = keywords[n].word;
            if (strlen(word) == token_length &&
                !memcmp(word, source + token_start, token_length)) {
                token = keywords[n].token;
                value = (uint8_t)(word[0] == 'T' ? 255 : 0);
                return;
            }
        }
        return;
    }
    if (digit(token) || token == '$' || token == '\'') {
        if (token == '\'') {
            if (source_size - cursor < 2)
                fail("incomplete character constant");
            value = source[cursor];
            cursor += 2;
        } else {
            unsigned base = token == '$' ? 16 : 10;
            if (base == 10)
                --cursor;
            value = 0;
            while (digit(peek()) || (base == 16 && peek() >= 'A' && peek() <= 'F')) {
                int c = source[cursor++];
                value = (uint8_t)(base * value + (digit(c) ? c - '0' : c - 'A' + 10));
            }
        }
        token = NUMBER;
    }
}

static void expect(int wanted)
{
    if (token != wanted)
        fail("unexpected token");
    next(false);
}

static void emit(uint8_t b)
{
    if (code_size == sizeof code)
        fail("generated code exceeds the address space");
    code[code_size++] = b;
}

static void instruction(uint8_t op, uint8_t operand)
{
    emit(op);
    emit(operand);
}

static void absolute(uint8_t op, unsigned addr)
{
    instruction(op, (uint8_t)(addr >> 8));
    emit((uint8_t)addr);
}

static unsigned pc(void) { return ORIGIN + (unsigned)code_size; }

/* $13B1: the main program uses the frame pointer at $82. */
static void frame_in_x(void)
{
    if (!x_current)
        instruction(0xde, 0x82);
    x_current = true;
}

static void patch(size_t offset)
{
    if (pc() > UINT16_MAX)
        fail("branch target exceeds the address space");
    code[offset] = (uint8_t)(pc() >> 8);
    code[offset + 1] = (uint8_t)pc();
}

static int closing_token(int open)
{
    switch (open) {
    case BEGIN: return END;
    case '(': return ')';
    case '[': return ']';
    case '{': return '}';
    default: return 0;
    }
}

enum OperandKind { IMMEDIATE, DIRECT, INDEXED, ACCUMULATOR };

struct Operand {
    enum OperandKind kind;
    uint8_t value;
};

static struct Operand parse_expression(void);
static void load_a(struct Operand operand);

/* $1732: a dynamic subscript is in A and value holds the array base. */
static struct Operand indexed_offset(void)
{
    uint8_t offset = value;
    if (token == VARIABLE) {
        next(false);
        return (struct Operand){INDEXED, offset};
    }
    expect(ARRAY_NAME);
    expect('[');
    struct Operand subscript = parse_expression();
    expect(']');
    if (subscript.kind == IMMEDIATE)
        return (struct Operand){INDEXED, (uint8_t)(offset + subscript.value)};
    load_a(subscript);
    return (struct Operand){ACCUMULATOR, offset};
}

/* $18AF: write the offset into the next indexed instruction. */
static void emit_array_access(uint8_t patch_opcode, uint8_t access_opcode)
{
    absolute(patch_opcode, pc() + 4);
    instruction(access_opcode, 0); /* The original leaves this byte untouched. */
}

/* $15EF: brackets may contain a single operand or a full expression. */
static struct Operand read_operand(void)
{
    if (token == '(' || token == '[' || token == '{') {
        int close = closing_token(token);
        next(false);
        struct Operand operand = parse_expression();
        expect(close);
        return operand;
    }

    if (token == MHIGH || token == MOD) {
        struct Operand operand = {DIRECT, token == MHIGH ? MHIGH_ADDR : MOD_ADDR};
        next(false);
        return operand;
    }

    uint8_t opcode = 0;
    unsigned routine = 0;
    switch (token) {
    case NEG: opcode = 0x40; break; /* NEGA */
    case COM: opcode = 0x43; break; /* COMA, also used for NOT */
    case LSR: opcode = 0x44; break; /* LSRA */
    case ROR: opcode = 0x46; break; /* RORA */
    case ASR: opcode = 0x47; break; /* ASRA */
    case ASL: opcode = 0x48; break; /* ASLA */
    case ROL: opcode = 0x49; break; /* ROLA */
    case RND: routine = RANDOM; break;
    case GET: routine = GET_CHAR; break;
    case READ: routine = READ_NUMBER; break;
    }
    if (opcode || routine) {
        next(false);
        expect('(');
        load_a(parse_expression());
        expect(')');
        if (routine) {
            if (routine != RANDOM)
                instruction(0x97, INPUT_DEVICE); /* STAA */
            absolute(0xbd, routine); /* JSR */
        } else {
            emit(opcode);
        }
        return (struct Operand){ACCUMULATOR, 0};
    }

    if (token == VARIABLE || token == ARRAY_NAME) {
        struct Operand offset = indexed_offset();
        if (offset.kind == ACCUMULATOR) {
            frame_in_x();
            instruction(0x8b, offset.value); /* ADDA */
            emit_array_access(0xb7, 0xa6); /* STAA, then LDAA */
        }
        return offset;
    }
    struct Operand operand = {IMMEDIATE, value};
    expect(NUMBER);
    return operand;
}

static void emit_operand(uint8_t opcode, struct Operand operand)
{
    if (operand.kind == INDEXED) {
        frame_in_x();
        opcode += 0x20; /* Immediate to indexed. */
    } else if (operand.kind == DIRECT) {
        opcode += 0x10; /* Immediate to direct. */
    }
    instruction(opcode, operand.value);
}

static void load_a(struct Operand operand)
{
    if (operand.kind != ACCUMULATOR)
        emit_operand(0x86, operand); /* LDAA */
}

static void right_operand(uint8_t opcode, struct Operand (*parse)(void))
{
    /* Reserve PSHA before parsing so absolute addresses stay in place. */
    emit(0x36);
    struct Operand operand = parse();
    if (operand.kind != ACCUMULATOR) {
        --code_size;
        emit_operand(opcode, operand);
        return;
    }

    if (opcode == 0x8b) {
        emit(0x33); /* PULB */
        emit(0x1b); /* ABA */
    } else if (opcode == 0x80 || opcode == 0x81 || opcode == 0xc6) {
        emit(0x16); /* TAB */
        emit(0x32); /* PULA */
        if (opcode != 0xc6)
            emit(opcode - 0x70); /* SBA or CBA */
    } else {
        instruction(0x97, 0x88); /* STAA $88 */
        emit(0x32); /* PULA */
        instruction(opcode + 0x10, 0x88); /* Direct mode */
    }
}

/* $15BB: * and / use A for the left value and B for the right value. */
static struct Operand term(void)
{
    struct Operand left = read_operand();
    while (token == '*' || token == '/') {
        unsigned routine = token == '*' ? 0x204d : 0x2069;
        load_a(left);
        next(false);
        right_operand(0xc6, read_operand); /* LDAB */
        absolute(0xbd, routine); /* JSR */
        left.kind = ACCUMULATOR;
    }
    return left;
}

/* $15A4: apply + and - from left to right. */
static struct Operand sum(void)
{
    struct Operand left = term();
    while (token == '+' || token == '-') {
        uint8_t opcode = token == '+' ? 0x8b : 0x80; /* ADDA or SUBA */
        load_a(left);
        next(false);
        right_operand(opcode, term);
        left.kind = ACCUMULATOR;
    }
    return left;
}

/* $1536: subtract, then use the branch condition to build a byte value. */
static struct Operand comparison(void)
{
    struct Operand left = sum();
    for (;;) {
        uint8_t branch;
        switch (token) {
        case '=': branch = 0x27; break; /* BEQ */
        case '#': branch = 0x26; break; /* BNE */
        case '<': branch = 0x25; break; /* BCS */
        case '>': branch = 0x22; break; /* BHI */
        case LT:  branch = 0x2d; break; /* BLT */
        case GT:  branch = 0x2e; break; /* BGT */
        default: return left;
        }
        load_a(left);
        next(false);
        right_operand(0x80, sum); /* SUBA or SBA */
        instruction(branch, 3);
        emit(0x4f); /* CLRA */
        instruction(0x20, 2); /* BRA */
        instruction(0x86, 0xff); /* LDAA #$FF */
        left.kind = ACCUMULATOR;
    }
}

/* $150A: AND, OR, EOR, ADC and SBC share a precedence level. */
static struct Operand parse_expression(void)
{
    struct Operand left = comparison();
    for (;;) {
        uint8_t opcode;
        switch (token) {
        case AND: opcode = 0x84; break;
        case OR:  opcode = 0x8a; break;
        case EOR: opcode = 0x88; break;
        case ADC: opcode = 0x89; break;
        case SBC: opcode = 0x82; break;
        default: return left;
        }
        load_a(left);
        next(false);
        right_operand(opcode, comparison);
        left.kind = ACCUMULATOR;
    }
}

static void expression(void)
{
    load_a(parse_expression());
}

static void statement(void);

/* $115F: evaluate the right side once, then store in reverse order. */
static void assignment(void)
{
    struct Operand offset = indexed_offset();
    if (offset.kind == ACCUMULATOR) {
        instruction(0x8b, offset.value); /* ADDA */
        emit(0x36); /* PSHA */
    }
    if (token == ',') {
        next(false);
        assignment();
    } else {
        expect(':');
        expect('=');
        expression();
    }
    frame_in_x();
    if (offset.kind == ACCUMULATOR) {
        emit(0x33); /* PULB */
        emit_array_access(0xf7, 0xa7); /* STAB, then STAA */
    } else {
        instruction(0xa7, offset.value);
    }
}

/* $1301: device selection precedes the list. Strings follow the JSR inline. */
static void write_statement(void)
{
    expect(WRITE);
    expect('(');
    expression();
    instruction(0x97, OUTPUT_DEVICE);
    expect(':');
    for (;;) {
        if (token == '"') {
            absolute(0xbd, PRINT_STRING);
            while (peek() != '"' && peek() != EOF_TOKEN)
                emit(source[cursor++]);
            if (peek() == EOF_TOKEN)
                fail("unterminated string");
            emit(0);
            ++cursor;
            next(false);
            x_current = false;
        } else if (token == ASCII || token == SPACE || token == CRLF) {
            int item = token;
            next(false);
            if (item == CRLF && token != '(') {
                absolute(0xbd, PRINT_NEWLINE);
            } else {
                expect('(');
                expression();
                expect(')');
                unsigned routine = PRINT_NEWLINES;
                if (item == ASCII)
                    routine = PRINT_CHAR;
                else if (item == SPACE)
                    routine = PRINT_SPACES;
                absolute(0xbd, routine);
            }
        } else {
            unsigned routine = PRINT_NUMBER;
            if (token == '#') {
                next(false);
                expect('(');
                expression();
                emit(0x36); /* PSHA: save the width. */
                expect(',');
                expression();
                expect(')');
                emit(0x33); /* PULB */
                routine = PRINT_WITH_WIDTH;
            } else {
                expression();
            }
            absolute(0xbd, routine);
            x_current = false;
        }
        if (token != ',')
            break;
        next(false);
    }
    expect(')');
}

/* $1105: one statement, including a compound statement. */
static void statement(void)
{
    int close = closing_token(token);
    if (close) {
        next(false);
        while (token != close)
            statement();
        next(false);
        return;
    }
    switch (token) {
    case VARIABLE:
    case ARRAY_NAME:
        assignment();
        break;
    case STOP_WORD:
        absolute(0x7e, STOP);
        next(false);
        break;
    case WRITE:
        write_statement();
        break;
    case CASE: {
        next(false);
        expression();
        expect(OF);
        size_t end_jump = 0;
        do {
            right_operand(0x81, parse_expression); /* CMPA or CBA */
            instruction(0x27, 3); /* BEQ: skip JMP to the next arm. */
            size_t next_arm = code_size + 1;
            absolute(0x7e, 0);
            bool test_x = x_current;
            statement();
            /* Link pending exit jumps through their address fields. */
            absolute(0x7e, end_jump);
            end_jump = code_size - 2;
            patch(next_arm);
            x_current = test_x;
        } while (token != ELSE);
        expect(ELSE);
        statement();
        x_current = false;
        while (end_jump) {
            size_t previous = ((size_t)code[end_jump] << 8) | code[end_jump + 1];
            patch(end_jump);
            end_jump = previous;
        }
        break;
    }
    case FOR: {
        next(false);
        if (token != VARIABLE)
            fail("expected a scalar loop variable");
        uint8_t offset = value;
        assignment();
        expect(TO);
        expression();
        expect(DO);
        unsigned start = pc();
        emit(0x36); /* PSHA: save the limit on each pass. */
        x_current = false;
        statement();
        emit(0x32); /* PULA */
        frame_in_x();
        instruction(0xa1, offset); /* CMPA */
        instruction(0x23, 5); /* BLS: skip INC and JMP. */
        instruction(0x6c, offset); /* INC */
        absolute(0x7e, start);
        break;
    }
    case IF:
    case WHILE: {
        bool loop = token == WHILE;
        unsigned start = pc();
        if (loop)
            x_current = false;
        next(false);
        expression();
        bool condition_x = x_current;
        expect(loop ? DO : THEN);
        instruction(0x26, 3);
        size_t target = code_size + 1;
        absolute(0x7e, 0);
        statement();
        if (loop)
            absolute(0x7e, start);
        x_current = loop ? condition_x : condition_x && x_current;
        patch(target);
        break;
    }
    case REPEAT: {
        unsigned start = pc();
        x_current = false;
        next(false);
        while (token != UNTIL)
            statement();
        expect(UNTIL);
        expression();
        instruction(0x26, 3);
        absolute(0x7e, start);
        break;
    }
    default:
        fail("expected a supported statement");
    }
}

/* $10AD: each name takes one byte plus its optional size. */
static uint8_t declarations(uint8_t offset)
{
    bool array = token == ARRAY;
    do {
        next(true);
        if (token != UNKNOWN)
            fail("expected a variable name");
        variables[variable_count++] = (struct Name){token_start, token_length, offset, array};
        ++offset;
        next(false);
        if (token == '[') {
            next(false);
            if (token != NUMBER || letter(source[token_start]))
                fail("expected a literal size");
            offset += value;
            next(false);
            expect(']');
        }
    } while (token == ',');
    return offset;
}

int main(void)
{
    int c;
    while ((c = getchar()) != EOF) {
        if (source_size == sizeof source)
            fail("source too large");
        if (c == 0)
            fail("NUL in source");
        if (c == '\n' && source_size && source[source_size - 1] == '\r')
            --source_size;
        source[source_size++] = (unsigned char)(c == '\n' ? 5 : c);
    }
    if (ferror(stdin))
        fail("cannot read source");
    next(false);
    uint8_t offset = 0;
    if (token == VAR)
        offset = declarations(offset);
    if (token == ARRAY)
        declarations(offset);
    statement();
    if (token != EOF_TOKEN)
        fail("unexpected input after main program");
    absolute(0x7e, STOP);

    puts("\t.code");
    for (size_t i = 0; i < code_size; ++i) {
        printf("%s0x%02x", i % 12 ? "," : "\t.byte ", (unsigned)code[i]);
        if (i % 12 == 11 || i + 1 == code_size)
            putchar('\n');
    }
    return fflush(stdout) == EOF ? EXIT_FAILURE : EXIT_SUCCESS;
}
