// Native Linux TL/1 compiler, first subset of the MB-6880 TL1.mot port.
// Usage: cc -std=c11 -O2 -o TL1 main.c; ./TL1 -o program.s program.tl1
// Assemble with Fuzix as6800, then link with ld6800 -b -C 0x2222.
// The generated section starts at $2222 and requires the original runtime.
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { ORIGIN = 0x2222, STOP = 0x209e, PRINT_NUMBER = 0x20a1,
       ENTER_FRAME = 0x200d, LEAVE_FRAME = 0x2025,
       PRINT_WITH_WIDTH = 0x20a2,
       PRINT_STRING = 0x2108, OUTPUT_DEVICE = 0x81,
       PRINT_CHAR = 0x2122, PRINT_SPACES = 0x20fd,
       PRINT_NEWLINE = 0x2111, PRINT_NEWLINES = 0x2119,
       RANDOM = 0x202f, GET_CHAR = 0x212e, READ_NUMBER = 0x20d8, INPUT_DEVICE = 0x80,
       MHIGH_ADDR = 0x86, MOD_ADDR = 0x87, MEM_ADDRESS = 0x88 };
enum Token { EOF_TOKEN = -1, NUMBER = 256, VARIABLE, UNKNOWN, VAR,
             BEGIN, END, WRITE, IF, THEN, WHILE, DO, REPEAT, UNTIL, STOP_WORD,
             LT, GT, AND, OR, EOR, ADC, SBC, NEG, COM, LSR, ROR, ASR, ASL, ROL,
             MHIGH, MOD, RND, GET, READ, ASCII, SPACE, CRLF, FOR, TO,
             CASE, OF, ELSE, ARRAY, ARRAY_NAME, MEM, CALL, CURSOR, TAB, PLOT,
             PROC, FUNC, PROCEDURE, FUNCTION, RETURN };

struct Name {
    size_t start, length;
    size_t offset;
    int token;
};

struct Subprogram {
    unsigned address;
    size_t pending;
};

enum Frame { NO_FRAME, LOCAL_FRAME = 0x82, GLOBAL_FRAME = 0x84 };
enum AddressingMode {
    MODE_NONE, MODE_INHERENT, MODE_IMMEDIATE, MODE_DIRECT,
    MODE_INDEXED, MODE_RELATIVE, MODE_EXTENDED
};

static unsigned char source[65536];
static uint8_t code[65536 - ORIGIN];
// Only the opcode byte has an addressing mode.
static uint8_t instruction_mode[sizeof code];
static struct Name variables[65536];
static size_t source_size, cursor, token_start, token_length, code_size, variable_count;
static struct Subprogram subprograms[sizeof source / 2];
static size_t subprogram_id, required_subprograms, global_names;
static int token;
static uint8_t value, frame_size;
static int subprogram_kind;
static enum Frame variable_frame, x_frame = LOCAL_FRAME;
static const char *source_path;

static void fail(const char *message)
{
    size_t line = 1;
    for (size_t i = 0; i < token_start; ++i)
        if (source[i] == 5)
            ++line;
    if (source_path)
        fprintf(stderr, "%s:%zu: %s\n", source_path, line, message);
    else
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

// $13FB: the declaration path reads the word without resolving it.
static void next(bool declaring)
{
    static const struct { const char *word; int token; } keywords[] = {
        {"VAR", VAR}, {"BEGIN", BEGIN}, {"END", END}, {"WRITE", WRITE},
        {"ARRAY", ARRAY}, {"PROC", PROC}, {"FUNC", FUNC}, {"RETURN", RETURN},
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
        {"MEM", MEM},
        {"RND", RND}, {"GET", GET}, {"READ", READ},
        {"ASCII", ASCII}, {"SPACE", SPACE}, {"CRLF", CRLF},
        {"CALL", CALL}, {"CURSOR", CURSOR}, {"TAB", TAB}, {"PLOT", PLOT},

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
                token = name.token;
                if (token == PROCEDURE || token == FUNCTION)
                    subprogram_id = name.offset;
                else {
                    value = (uint8_t)name.offset;
                    variable_frame = subprogram_kind && n <= global_names ? GLOBAL_FRAME : LOCAL_FRAME;
                }
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

static void emit_byte(uint8_t b)
{
    if (code_size == sizeof code)
        fail("generated code exceeds the address space");
    instruction_mode[code_size] = MODE_NONE;
    code[code_size++] = b;
}

static void emit_inherent(uint8_t op)
{
    emit_byte(op);
    instruction_mode[code_size - 1] = MODE_INHERENT;
}

static void emit_with_byte_operand(uint8_t op, uint8_t operand, enum AddressingMode mode)
{
    emit_byte(op);
    emit_byte(operand);
    instruction_mode[code_size - 2] = mode;
}

static void emit_immediate(uint8_t op, uint8_t value)
{
    emit_with_byte_operand(op, value, MODE_IMMEDIATE);
}

static void emit_direct(uint8_t op, uint8_t addr)
{
    emit_with_byte_operand(op, addr, MODE_DIRECT);
}

static void emit_indexed(uint8_t op, uint8_t offset)
{
    emit_with_byte_operand(op, offset, MODE_INDEXED);
}

static void emit_relative(uint8_t op, int8_t displacement)
{
    emit_with_byte_operand(op, (uint8_t)displacement, MODE_RELATIVE);
}

static void emit_extended(uint8_t op, unsigned addr)
{
    emit_byte(op);
    emit_byte((uint8_t)(addr >> 8));
    emit_byte((uint8_t)addr);
    instruction_mode[code_size - 3] = MODE_EXTENDED;
}

static unsigned pc(void) { return ORIGIN + (unsigned)code_size; }

// $13B1: X can hold the local frame or the main program's frame.
static void frame_in_x(enum Frame frame)
{
    if (x_frame != frame)
        emit_direct(0xde, frame);
    x_frame = frame;
}

static void patch(size_t offset)
{
    if (pc() > UINT16_MAX)
        fail("branch target exceeds the address space");
    code[offset] = (uint8_t)(pc() >> 8);
    code[offset + 1] = (uint8_t)pc();
}

static void patch_jumps(size_t jump)
{
    while (jump) {
        size_t previous = ((size_t)code[jump] << 8) | code[jump + 1];
        patch(jump);
        jump = previous;
    }
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
    enum Frame frame;
};

static struct Operand parse_expression(void);
static void load_a(struct Operand operand);

// $1732: a dynamic subscript is in A and value holds the array base.
static struct Operand indexed_offset(void)
{
    uint8_t offset = value;
    enum Frame frame = variable_frame;
    if (token == VARIABLE) {
        next(false);
        return (struct Operand){INDEXED, offset, frame};
    }
    expect(ARRAY_NAME);
    expect('[');
    struct Operand subscript = parse_expression();
    expect(']');
    if (subscript.kind == IMMEDIATE)
        return (struct Operand){INDEXED, (uint8_t)(offset + subscript.value), frame};
    load_a(subscript);
    return (struct Operand){ACCUMULATOR, offset, frame};
}

// $18AF: write the offset into the next indexed instruction.
static void emit_array_access(uint8_t patch_opcode, uint8_t access_opcode)
{
    emit_extended(patch_opcode, pc() + 4);
    emit_indexed(access_opcode, 0); // The original leaves this byte untouched.
}

// $1718: leave the high byte on the stack and the low byte in A.
static void memory_address(void)
{
    expect(MEM);
    expect('(');
    load_a(parse_expression());
    emit_inherent(0x36); // PSHA
    expect(',');
    load_a(parse_expression());
    expect(')');
}

// $1649: arguments follow the two bytes reserved for the saved frame pointer.
static void call_arguments(void)
{
    next(false);
    uint8_t caller_size = frame_size;
    if (token == '(') {
        ++frame_size;
        do {
            next(false);
            ++frame_size;
            load_a(parse_expression());
            frame_in_x(LOCAL_FRAME);
            emit_indexed(0xa7, frame_size); // STAA
        } while (token == ',');
        expect(')');
        frame_size = caller_size;
    }
    emit_immediate(0x86, caller_size); // LDAA
    x_frame = LOCAL_FRAME;
}

static void builtin_call(void)
{
    static const unsigned routines[] = {0x216f, 0x217b, 0x2185, 0x218f};
    unsigned routine = routines[token - CALL];
    call_arguments();
    emit_extended(0xbd, routine);
}

// $1847: link calls until the subprogram address is known.
static void subprogram_call(void)
{
    size_t id = subprogram_id;
    struct Subprogram *subprogram = &subprograms[id];
    if (required_subprograms <= id)
        required_subprograms = id + 1;
    call_arguments();
    if (subprogram->address) {
        emit_extended(0xbd, subprogram->address);
    } else {
        emit_extended(0xbd, subprogram->pending);
        subprogram->pending = code_size - 2;
    }
}

// $15EF: brackets may contain a single operand or a full expression.
static struct Operand read_operand(void)
{
    if (token == '(' || token == '[' || token == '{') {
        int close = closing_token(token);
        next(false);
        struct Operand operand = parse_expression();
        expect(close);
        return operand;
    }

    if (token == MEM) {
        memory_address();
        emit_direct(0x97, MEM_ADDRESS + 1); // STAA
        emit_inherent(0x32); // PULA
        emit_direct(0x97, MEM_ADDRESS); // STAA
        emit_direct(0xde, MEM_ADDRESS); // LDX
        emit_indexed(0xa6, 0); // LDAA ,X
        x_frame = NO_FRAME;
        return (struct Operand){ACCUMULATOR, 0, NO_FRAME};
    }

    if (token == MHIGH || token == MOD) {
        struct Operand operand = {DIRECT, token == MHIGH ? MHIGH_ADDR : MOD_ADDR, NO_FRAME};
        next(false);
        return operand;
    }

    uint8_t opcode = 0;
    unsigned routine = 0;
    switch (token) {
    case FUNCTION:
        subprogram_call();
        return (struct Operand){ACCUMULATOR, 0, NO_FRAME};
    case NEG: opcode = 0x40; break; // NEGA
    case COM: opcode = 0x43; break; // COMA, also used for NOT
    case LSR: opcode = 0x44; break; // LSRA
    case ROR: opcode = 0x46; break; // RORA
    case ASR: opcode = 0x47; break; // ASRA
    case ASL: opcode = 0x48; break; // ASLA
    case ROL: opcode = 0x49; break; // ROLA
    case RND: routine = RANDOM; break;
    case GET: routine = GET_CHAR; break;
    case READ: routine = READ_NUMBER; break;
    case CALL:
    case CURSOR:
    case TAB:
    case PLOT:
        builtin_call();
        return (struct Operand){ACCUMULATOR, 0, NO_FRAME};
    }
    if (opcode || routine) {
        next(false);
        expect('(');
        load_a(parse_expression());
        expect(')');
        if (routine) {
            if (routine != RANDOM)
                emit_direct(0x97, INPUT_DEVICE); // STAA
            emit_extended(0xbd, routine); // JSR
        } else {
            emit_inherent(opcode);
        }
        return (struct Operand){ACCUMULATOR, 0, NO_FRAME};
    }

    if (token == VARIABLE || token == ARRAY_NAME) {
        struct Operand offset = indexed_offset();
        if (offset.kind == ACCUMULATOR) {
            frame_in_x(offset.frame);
            emit_immediate(0x8b, offset.value); // ADDA
            emit_array_access(0xb7, 0xa6); // STAA, then LDAA
        }
        return offset;
    }
    struct Operand operand = {IMMEDIATE, value, NO_FRAME};
    expect(NUMBER);
    return operand;
}

static void emit_operand(uint8_t opcode, struct Operand operand)
{
    if (operand.kind == INDEXED) {
        frame_in_x(operand.frame);
        emit_indexed(opcode + 0x20, operand.value);
    } else if (operand.kind == DIRECT) {
        emit_direct(opcode + 0x10, operand.value);
    } else {
        emit_immediate(opcode, operand.value);
    }
}

static void load_a(struct Operand operand)
{
    if (operand.kind != ACCUMULATOR)
        emit_operand(0x86, operand); // LDAA
}

static void right_operand(uint8_t opcode, struct Operand (*parse)(void))
{
    // Reserve PSHA before parsing so absolute addresses stay in place.
    emit_inherent(0x36);
    struct Operand operand = parse();
    if (operand.kind != ACCUMULATOR) {
        --code_size;
        emit_operand(opcode, operand);
        return;
    }

    if (opcode == 0x8b) {
        emit_inherent(0x33); // PULB
        emit_inherent(0x1b); // ABA
    } else if (opcode == 0x80 || opcode == 0x81 || opcode == 0xc6) {
        emit_inherent(0x16); // TAB
        emit_inherent(0x32); // PULA
        if (opcode != 0xc6)
            emit_inherent(opcode - 0x70); // SBA or CBA
    } else {
        emit_direct(0x97, 0x88); // STAA $88
        emit_inherent(0x32); // PULA
        emit_direct(opcode + 0x10, 0x88); // Direct mode
    }
}

// $15BB: * and / use A for the left value and B for the right value.
static struct Operand term(void)
{
    struct Operand left = read_operand();
    while (token == '*' || token == '/') {
        unsigned routine = token == '*' ? 0x204d : 0x2069;
        load_a(left);
        next(false);
        right_operand(0xc6, read_operand); // LDAB
        emit_extended(0xbd, routine); // JSR
        left.kind = ACCUMULATOR;
    }
    return left;
}

// $15A4: apply + and - from left to right.
static struct Operand sum(void)
{
    struct Operand left = term();
    while (token == '+' || token == '-') {
        uint8_t opcode = token == '+' ? 0x8b : 0x80; // ADDA or SUBA
        load_a(left);
        next(false);
        right_operand(opcode, term);
        left.kind = ACCUMULATOR;
    }
    return left;
}

// $1536: subtract, then use the branch condition to build a byte value.
static struct Operand comparison(void)
{
    struct Operand left = sum();
    for (;;) {
        uint8_t branch;
        switch (token) {
        case '=': branch = 0x27; break; // BEQ
        case '#': branch = 0x26; break; // BNE
        case '<': branch = 0x25; break; // BCS
        case '>': branch = 0x22; break; // BHI
        case LT:  branch = 0x2d; break; // BLT
        case GT:  branch = 0x2e; break; // BGT
        default: return left;
        }
        load_a(left);
        next(false);
        right_operand(0x80, sum); // SUBA or SBA
        emit_relative(branch, 3);
        emit_inherent(0x4f); // CLRA
        emit_relative(0x20, 2); // BRA
        emit_immediate(0x86, 0xff); // LDAA #$FF
        left.kind = ACCUMULATOR;
    }
}

// $150A: AND, OR, EOR, ADC and SBC share a precedence level.
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

// $115F: evaluate the right side once, then store in reverse order.
static void assignment(void)
{
    bool memory = token == MEM;
    struct Operand offset = {0};
    if (memory) {
        memory_address();
        emit_inherent(0x36); // PSHA
    } else {
        offset = indexed_offset();
        if (offset.kind == ACCUMULATOR) {
            emit_immediate(0x8b, offset.value); // ADDA
            emit_inherent(0x36); // PSHA
        }
    }
    if (token == ',') {
        next(false);
        assignment();
    } else {
        expect(':');
        expect('=');
        expression();
    }
    if (memory) {
        emit_inherent(0x33); // PULB
        emit_direct(0xd7, MEM_ADDRESS + 1); // STAB
        emit_inherent(0x33); // PULB
        emit_direct(0xd7, MEM_ADDRESS); // STAB
        emit_direct(0xde, MEM_ADDRESS); // LDX
        emit_indexed(0xa7, 0); // STAA ,X
        x_frame = NO_FRAME;
        return;
    }
    frame_in_x(offset.frame);
    if (offset.kind == ACCUMULATOR) {
        emit_inherent(0x33); // PULB
        emit_array_access(0xf7, 0xa7); // STAB, then STAA
    } else {
        emit_indexed(0xa7, offset.value);
    }
}

// $1301: device selection precedes the list. Strings follow the JSR inline.
static void write_statement(void)
{
    expect(WRITE);
    expect('(');
    expression();
    emit_direct(0x97, OUTPUT_DEVICE);
    expect(':');
    for (;;) {
        if (token == '"') {
            emit_extended(0xbd, PRINT_STRING);
            while (peek() != '"' && peek() != EOF_TOKEN)
                emit_byte(source[cursor++]);
            if (peek() == EOF_TOKEN)
                fail("unterminated string");
            emit_byte(0);
            ++cursor;
            next(false);
            x_frame = NO_FRAME;
        } else if (token == ASCII || token == SPACE || token == CRLF) {
            int item = token;
            next(false);
            if (item == CRLF && token != '(') {
                emit_extended(0xbd, PRINT_NEWLINE);
            } else {
                expect('(');
                expression();
                expect(')');
                unsigned routine = PRINT_NEWLINES;
                if (item == ASCII)
                    routine = PRINT_CHAR;
                else if (item == SPACE)
                    routine = PRINT_SPACES;
                emit_extended(0xbd, routine);
            }
        } else {
            unsigned routine = PRINT_NUMBER;
            if (token == '#') {
                next(false);
                expect('(');
                expression();
                emit_inherent(0x36); // PSHA: save the width.
                expect(',');
                expression();
                expect(')');
                emit_inherent(0x33); // PULB
                routine = PRINT_WITH_WIDTH;
            } else {
                expression();
            }
            emit_extended(0xbd, routine);
            x_frame = NO_FRAME;
        }
        if (token != ',')
            break;
        next(false);
    }
    expect(')');
}

// $1105: one statement, including a compound statement.
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
    case MEM:
        assignment();
        break;
    case STOP_WORD:
        emit_extended(0x7e, STOP);
        next(false);
        break;
    case RETURN:
        if (!subprogram_kind)
            fail("RETURN outside a subprogram");
        next(false);
        if (subprogram_kind == FUNCTION)
            expression();
        emit_extended(0x7e, LEAVE_FRAME);
        break;
    case PROCEDURE:
        subprogram_call();
        break;
    case WRITE:
        write_statement();
        break;
    case CALL:
    case CURSOR:
    case TAB:
    case PLOT:
        builtin_call();
        break;
    case CASE: {
        next(false);
        expression();
        expect(OF);
        size_t end_jump = 0;
        do {
            right_operand(0x81, parse_expression); // CMPA or CBA
            emit_relative(0x27, 3); // BEQ: skip JMP to the next arm.
            size_t next_arm = code_size + 1;
            emit_extended(0x7e, 0);
            enum Frame test_x = x_frame;
            statement();
            // Link pending exit jumps through their address fields.
            emit_extended(0x7e, end_jump);
            end_jump = code_size - 2;
            patch(next_arm);
            x_frame = test_x;
        } while (token != ELSE);
        expect(ELSE);
        statement();
        x_frame = NO_FRAME;
        patch_jumps(end_jump);
        break;
    }
    case FOR: {
        next(false);
        if (token != VARIABLE)
            fail("expected a scalar loop variable");
        uint8_t offset = value;
        enum Frame frame = variable_frame;
        assignment();
        expect(TO);
        expression();
        expect(DO);
        unsigned start = pc();
        emit_inherent(0x36); // PSHA: save the limit on each pass.
        x_frame = NO_FRAME;
        statement();
        emit_inherent(0x32); // PULA
        frame_in_x(frame);
        emit_indexed(0xa1, offset); // CMPA
        emit_relative(0x23, 5); // BLS: skip INC and JMP.
        emit_indexed(0x6c, offset); // INC
        emit_extended(0x7e, start);
        break;
    }
    case IF:
    case WHILE: {
        bool loop = token == WHILE;
        unsigned start = pc();
        if (loop)
            x_frame = NO_FRAME;
        next(false);
        expression();
        enum Frame condition_frame = x_frame;
        expect(loop ? DO : THEN);
        emit_relative(0x26, 3);
        size_t target = code_size + 1;
        emit_extended(0x7e, 0);
        statement();
        if (loop)
            emit_extended(0x7e, start);
        if (loop)
            x_frame = condition_frame;
        else if (x_frame != condition_frame)
            x_frame = NO_FRAME;
        patch(target);
        break;
    }
    case REPEAT: {
        unsigned start = pc();
        x_frame = NO_FRAME;
        next(false);
        while (token != UNTIL)
            statement();
        expect(UNTIL);
        expression();
        emit_relative(0x26, 3);
        emit_extended(0x7e, start);
        break;
    }
    default:
        fail("expected a supported statement");
    }
}

// $10AD: each name takes one byte plus its optional size.
static size_t declarations(size_t offset)
{
    int kind = VARIABLE;
    if (token == ARRAY)
        kind = ARRAY_NAME;
    else if (token == PROC)
        kind = PROCEDURE;
    else if (token == FUNC)
        kind = FUNCTION;
    do {
        next(true);
        if (token != UNKNOWN)
            fail("expected a variable name");
        variables[variable_count++] = (struct Name){token_start, token_length, offset, kind};
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

// $105E: subprogram definitions follow the main program.
static void define_subprogram(void)
{
    struct Subprogram *subprogram = &subprograms[subprogram_id];
    if (subprogram->address)
        fail("subprogram already defined");
    subprogram->address = pc();
    patch_jumps(subprogram->pending);
    emit_extended(0xbd, ENTER_FRAME);
    subprogram_kind = token;
    next(false);
    frame_size = 0;
    x_frame = LOCAL_FRAME;
    if (token == '(') {
        frame_size = declarations(frame_size);
        expect(')');
    }
    if (token == VAR)
        frame_size = declarations(frame_size);
    if (token == ARRAY)
        frame_size = declarations(frame_size);
    statement();
    if (subprogram_kind == PROCEDURE)
        emit_extended(0x7e, LEAVE_FRAME);
    variable_count = global_names;
}

static FILE *open_output(const char *path, FILE *input)
{
    // Check the opened file before truncating it.
    int fd = open(path, O_WRONLY | O_CREAT, 0666);
    if (fd == -1) {
        perror(path);
        return NULL;
    }
    FILE *output = fdopen(fd, "w");
    if (!output) {
        perror(path);
        close(fd);
        return NULL;
    }

    struct stat input_stat, output_stat;
    if (fstat(fileno(input), &input_stat) == -1 || fstat(fd, &output_stat) == -1) {
        perror(path);
    } else if (!S_ISREG(output_stat.st_mode)) {
        return output;
    } else if (input_stat.st_dev == output_stat.st_dev && input_stat.st_ino == output_stat.st_ino) {
        fprintf(stderr, "TL1: %s is the input file\n", path);
    } else if (ftruncate(fd, 0) == 0) {
        return output;
    } else {
        perror(path);
    }
    fclose(output);
    return NULL;
}

int main(int argc, char **argv)
{
    const char *output_path = NULL;
    bool options = true;
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (options && strcmp(arg, "--") == 0) {
            options = false;
        } else if (options && strcmp(arg, "-o") == 0 && !output_path && i + 1 < argc) {
            output_path = argv[++i];
        } else if (!source_path && (!options || arg[0] != '-')) {
            source_path = arg;
        } else {
            fputs("Usage: TL1 [-o output.s] [input.tl1]\n", stderr);
            return EXIT_FAILURE;
        }
    }

    FILE *input = stdin;
    if (source_path) {
        input = fopen(source_path, "r");
        if (!input) {
            perror(source_path);
            return EXIT_FAILURE;
        }
    }
    int c;
    while ((c = getc(input)) != EOF) {
        if (source_size == sizeof source)
            fail("source too large");
        if (c == 0)
            fail("NUL in source");
        if (c == '\n' && source_size && source[source_size - 1] == '\r')
            --source_size;
        source[source_size++] = (unsigned char)(c == '\n' ? 5 : c);
    }
    if (ferror(input)) {
        perror(source_path ? source_path : "TL1: stdin");
        return EXIT_FAILURE;
    }

    next(false);
    size_t subprogram_count = 0;
    if (token == PROC)
        subprogram_count = declarations(0);
    if (token == FUNC)
        subprogram_count = declarations(subprogram_count);
    if (subprogram_count > sizeof subprograms / sizeof *subprograms)
        fail("too many subprograms");
    if (token == VAR)
        frame_size = declarations(frame_size);
    if (token == ARRAY)
        frame_size = declarations(frame_size);
    global_names = variable_count;
    statement();
    emit_extended(0x7e, STOP);
    while (token == PROCEDURE || token == FUNCTION)
        define_subprogram();
    // $105E: the original compiler stops at a top-level slash.
    if (token != EOF_TOKEN && token != '/')
        fail("unexpected input after main program");
    for (size_t id = 0; id < required_subprograms; ++id)
        if (!subprograms[id].address)
            fail("undefined subprogram");

    FILE *output = stdout;
    if (output_path) {
        output = open_output(output_path, input);
        if (!output)
            return EXIT_FAILURE;
    }
    if (source_path && fclose(input) == EOF) {
        perror(source_path);
        return EXIT_FAILURE;
    }
    fputs("\t.code\n", output);
    static const uint8_t byte_count[] = {
        [MODE_NONE] = 1, [MODE_INHERENT] = 1, [MODE_IMMEDIATE] = 2,
        [MODE_DIRECT] = 2, [MODE_INDEXED] = 2, [MODE_RELATIVE] = 2,
        [MODE_EXTENDED] = 3,
    };
    enum { BYTES_PER_LINE = 12 };
    unsigned line_bytes = 0;
    for (size_t i = 0; i < code_size; ++i) {
        if (line_bytes + byte_count[instruction_mode[i]] > BYTES_PER_LINE) {
            putc('\n', output);
            line_bytes = 0;
        }
        fprintf(output, "%s0x%02x", line_bytes ? "," : "\t.byte ", (unsigned)code[i]);
        ++line_bytes;
    }
    if (line_bytes)
        putc('\n', output);
    if (ferror(output) || (output_path ? fclose(output) : fflush(output)) == EOF) {
        perror(output_path ? output_path : "TL1: stdout");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
