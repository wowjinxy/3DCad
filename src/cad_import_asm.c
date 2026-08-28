#include "cad_import_asm.h"

#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct AsmLine {
    char* text;
    size_t byteOffset;
    size_t lineNumber;
} AsmLine;

typedef struct AsmSourceView {
    char* storage;
    AsmLine* lines;
    size_t lineCount;
} AsmSourceView;

typedef struct AsmConstant {
    char name[64];
    int value;
} AsmConstant;

typedef struct AsmConstantTable {
    AsmConstant entries[CAD_ASM_MAX_CONSTANTS];
    size_t count;
} AsmConstantTable;

typedef struct AsmExpression {
    const char* cursor;
    const AsmConstantTable* constants;
    int failed;
} AsmExpression;

typedef struct AsmVertex {
    double x;
    double y;
    double z;
} AsmVertex;

static int ascii_tolower(int c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

static int ascii_compare_ci(const char* left, const char* right) {
    unsigned char a;
    unsigned char b;
    if (!left) left = "";
    if (!right) right = "";
    do {
        a = (unsigned char)ascii_tolower((unsigned char)*left++);
        b = (unsigned char)ascii_tolower((unsigned char)*right++);
        if (a != b) return a < b ? -1 : 1;
    } while (a != 0);
    return 0;
}

static int ascii_equal_ci(const char* left, const char* right) {
    return ascii_compare_ci(left, right) == 0;
}

static int ascii_starts_ci(const char* text, const char* prefix) {
    if (!text || !prefix) return 0;
    while (*prefix) {
        if (!*text || ascii_tolower((unsigned char)*text) !=
                          ascii_tolower((unsigned char)*prefix)) return 0;
        ++text;
        ++prefix;
    }
    return 1;
}

static size_t bounded_length(const char* text, size_t capacity) {
    size_t length = 0;
    if (!text) return 0;
    while (length < capacity && text[length]) ++length;
    return length;
}

static void copy_bounded(char* output, size_t capacity, const char* input) {
    size_t length;
    if (!output || capacity == 0) return;
    if (!input) input = "";
    length = bounded_length(input, capacity - 1);
    memcpy(output, input, length);
    output[length] = '\0';
}

static CadResult asm_result(void) {
    return CadResult_Ok(CAD_FORMAT_AUTO);
}

static void asm_add_diagnostic(CadResult* result,
                               CadDiagnosticSeverity severity,
                               CadStatus code, size_t byteOffset,
                               size_t lineNumber, const char* format, ...) {
    va_list args;
    CadDiagnostic* diagnostic;
    if (!result) return;
    if (severity == CAD_DIAGNOSTIC_WARNING) result->warningCount++;
    if (severity == CAD_DIAGNOSTIC_ERROR) {
        result->errorCount++;
        if (result->status == CAD_STATUS_OK) result->status = code;
    }
    if (result->diagnosticCount >= CAD_DIAGNOSTIC_CAPACITY) return;
    diagnostic = &result->diagnostics[result->diagnosticCount++];
    memset(diagnostic, 0, sizeof(*diagnostic));
    diagnostic->severity = severity;
    diagnostic->code = code;
    diagnostic->byteOffset = byteOffset;
    diagnostic->recordTag = -1;
    diagnostic->recordIndex = lineNumber > (size_t)INT_MAX
                                  ? INT_MAX : (int)lineNumber;
    va_start(args, format);
    vsnprintf(diagnostic->message, sizeof(diagnostic->message), format, args);
    va_end(args);
}

static void free_source_view(AsmSourceView* source) {
    if (!source) return;
    free(source->lines);
    free(source->storage);
    memset(source, 0, sizeof(*source));
}

static int make_source_view(const CadAsmTextSource* source,
                            AsmSourceView* output, CadResult* result) {
    size_t lineCapacity = 1;
    size_t index;
    size_t start;
    if (!source || !output || (!source->bytes && source->size)) {
        asm_add_diagnostic(result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_INVALID_ARGUMENT, 0, 0,
                           "ASM source and output must be valid");
        return 0;
    }
    memset(output, 0, sizeof(*output));
    if (source->size == 0) {
        asm_add_diagnostic(result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_EMPTY_INPUT, 0, 0,
                           "ASM source '%s' is empty",
                           source->name ? source->name : "(unnamed)");
        return 0;
    }
    if (source->size > CAD_ASM_MAX_INPUT_BYTES) {
        asm_add_diagnostic(result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_INVALID_ARGUMENT, source->size, 0,
                           "ASM source '%s' exceeds the %u-byte safety limit",
                           source->name ? source->name : "(unnamed)",
                           (unsigned)CAD_ASM_MAX_INPUT_BYTES);
        return 0;
    }
    for (index = 0; index < source->size; ++index) {
        if (source->bytes[index] == '\0') {
            asm_add_diagnostic(result, CAD_DIAGNOSTIC_ERROR,
                               CAD_STATUS_INVALID_NUMBER, index, 0,
                               "ASM source '%s' contains an embedded NUL byte",
                               source->name ? source->name : "(unnamed)");
            return 0;
        }
        if (source->bytes[index] == '\n' ||
            (source->bytes[index] == '\r' &&
             (index + 1 == source->size || source->bytes[index + 1] != '\n')))
            ++lineCapacity;
    }
    output->storage = (char*)malloc(source->size + 1);
    output->lines = (AsmLine*)calloc(lineCapacity, sizeof(*output->lines));
    if (!output->storage || !output->lines) {
        free_source_view(output);
        asm_add_diagnostic(result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_OUT_OF_MEMORY, 0, 0,
                           "Could not allocate ASM parser workspace");
        return 0;
    }
    memcpy(output->storage, source->bytes, source->size);
    output->storage[source->size] = '\0';
    start = 0;
    index = 0;
    while (index <= source->size) {
        if (index == source->size || output->storage[index] == '\n' ||
            output->storage[index] == '\r') {
            char separator = output->storage[index];
            char* comment;
            char* end;
            output->storage[index] = '\0';
            output->lines[output->lineCount].text = output->storage + start;
            output->lines[output->lineCount].byteOffset = start;
            output->lines[output->lineCount].lineNumber =
                output->lineCount + 1;
            comment = strchr(output->storage + start, ';');
            if (comment) *comment = '\0';
            end = output->storage + index;
            while (end > output->storage + start &&
                   isspace((unsigned char)end[-1])) *--end = '\0';
            ++output->lineCount;
            if (index == source->size) break;
            ++index;
            if (separator == '\r' && index < source->size &&
                output->storage[index] == '\n') {
                output->storage[index] = '\0';
                ++index;
            }
            start = index;
            continue;
        }
        ++index;
    }
    return 1;
}

static const char* skip_space_const(const char* text) {
    while (text && *text && isspace((unsigned char)*text)) ++text;
    return text;
}

static int identifier_character(int value) {
    return isalnum((unsigned char)value) || value == '_' || value == '.';
}

static int next_word(const char** cursor, char* output, size_t capacity,
                     const char** wordStart) {
    const char* position;
    size_t length = 0;
    if (!cursor || !*cursor || !output || capacity == 0) return 0;
    position = skip_space_const(*cursor);
    while (*position && !identifier_character((unsigned char)*position)) {
        if (*position == ',' || *position == '<' || *position == '>') {
            *cursor = position;
            output[0] = '\0';
            return 0;
        }
        ++position;
        position = skip_space_const(position);
    }
    if (!*position) {
        *cursor = position;
        output[0] = '\0';
        return 0;
    }
    if (wordStart) *wordStart = position;
    while (identifier_character((unsigned char)*position)) {
        if (length + 1 < capacity) output[length++] = *position;
        ++position;
    }
    output[length] = '\0';
    *cursor = position;
    return length != 0;
}

static const char* find_word_ci(const char* line, const char* wanted,
                                char* actual, size_t actualCapacity) {
    const char* cursor = line;
    const char* start = NULL;
    char word[CAD_ASM_NAME_CAPACITY];
    while (next_word(&cursor, word, sizeof(word), &start)) {
        if (ascii_equal_ci(word, wanted)) {
            if (actual) copy_bounded(actual, actualCapacity, word);
            return start;
        }
        cursor = skip_space_const(cursor);
        if (*cursor == ',') ++cursor;
    }
    return NULL;
}

static int first_word(const char* line, char* output, size_t capacity) {
    const char* cursor = line;
    return next_word(&cursor, output, capacity, NULL);
}

static int suffix_ci(const char* text, const char* suffix) {
    size_t textLength;
    size_t suffixLength;
    if (!text || !suffix) return 0;
    textLength = strlen(text);
    suffixLength = strlen(suffix);
    return textLength >= suffixLength &&
           ascii_equal_ci(text + textLength - suffixLength, suffix);
}

static void trim_copy_range(char* output, size_t capacity,
                            const char* begin, const char* end) {
    size_t length;
    if (!output || capacity == 0) return;
    while (begin < end && isspace((unsigned char)*begin)) ++begin;
    while (end > begin && isspace((unsigned char)end[-1])) --end;
    length = (size_t)(end - begin);
    if (length >= capacity) length = capacity - 1;
    memcpy(output, begin, length);
    output[length] = '\0';
}

static int header_sections(const char* line, char* points, size_t pointsCapacity,
                           char* faces, size_t facesCapacity,
                           char* label, size_t labelCapacity) {
    const char* macro = find_word_ci(line, "ShapeHdr", NULL, 0);
    const char* cursor;
    const char* comma;
    const char* prefixEnd;
    char first[CAD_ASM_NAME_CAPACITY];
    if (!macro) return 0;
    prefixEnd = macro;
    trim_copy_range(label, labelCapacity, line, prefixEnd);
    if (label[0]) {
        const char* labelCursor = label;
        if (!next_word(&labelCursor, first, sizeof(first), NULL)) label[0] = '\0';
        else copy_bounded(label, labelCapacity, first);
    }
    cursor = macro + strlen("ShapeHdr");
    cursor = skip_space_const(cursor);
    comma = strchr(cursor, ',');
    if (!comma) return 0;
    trim_copy_range(points, pointsCapacity, cursor, comma);
    cursor = comma + 1;
    comma = strchr(cursor, ',');
    if (!comma) return 0;
    cursor = comma + 1;
    comma = strchr(cursor, ',');
    if (!comma) return 0;
    trim_copy_range(faces, facesCapacity, cursor, comma);
    return points[0] && faces[0] && !ascii_equal_ci(points, "0") &&
           !ascii_equal_ci(faces, "0");
}

static int catalog_has_alias(const CadAsmCatalog* catalog,
                             size_t sourceIndex, const char* name,
                             const char* pointSection) {
    size_t index;
    for (index = 0; index < catalog->shapeCount; ++index) {
        if (catalog->shapes[index].sourceIndex == sourceIndex &&
            ascii_equal_ci(catalog->shapes[index].name, name) &&
            ascii_equal_ci(catalog->shapes[index].pointSection, pointSection))
            return 1;
    }
    return 0;
}

static int face_section_label(const char* line, const char* base);

static int add_catalog_entry(CadAsmCatalog* catalog, size_t sourceIndex,
                             const char* sourceName, const AsmLine* line,
                             const char* name, const char* points,
                             const char* faces, CadResult* result) {
    CadAsmShapeInfo* shape;
    if (catalog->shapeCount >= CAD_ASM_MAX_CATALOG_SHAPES) {
        asm_add_diagnostic(result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_INDEX_OUT_OF_RANGE,
                           line ? line->byteOffset : 0,
                           line ? line->lineNumber : 0,
                           "ASM catalog exceeds the %u-shape limit",
                           (unsigned)CAD_ASM_MAX_CATALOG_SHAPES);
        return 0;
    }
    shape = &catalog->shapes[catalog->shapeCount++];
    memset(shape, 0, sizeof(*shape));
    copy_bounded(shape->name, sizeof(shape->name), name);
    copy_bounded(shape->pointSection, sizeof(shape->pointSection), points);
    copy_bounded(shape->faceSection, sizeof(shape->faceSection), faces);
    copy_bounded(shape->sourceName, sizeof(shape->sourceName), sourceName);
    shape->sourceIndex = sourceIndex;
    if (line) {
        shape->byteOffset = line->byteOffset;
        shape->lineNumber = line->lineNumber;
    }
    return 1;
}

static int compare_shape_info(const void* leftValue, const void* rightValue) {
    const CadAsmShapeInfo* left = (const CadAsmShapeInfo*)leftValue;
    const CadAsmShapeInfo* right = (const CadAsmShapeInfo*)rightValue;
    int comparison = ascii_compare_ci(left->name, right->name);
    if (comparison) return comparison;
    comparison = strcmp(left->name, right->name);
    if (comparison) return comparison;
    comparison = ascii_compare_ci(left->sourceName, right->sourceName);
    if (comparison) return comparison;
    if (left->byteOffset != right->byteOffset)
        return left->byteOffset < right->byteOffset ? -1 : 1;
    if (left->sourceIndex != right->sourceIndex)
        return left->sourceIndex < right->sourceIndex ? -1 : 1;
    return 0;
}

CadAsmImportOptions CadImportAsm_DefaultOptions(void) {
    CadAsmImportOptions options;
    options.invertY = 1;
    return options;
}

CadResult CadImportAsm_BuildCatalog(const CadAsmTextSource* asmSources,
                                    size_t asmSourceCount,
                                    CadAsmCatalog* output) {
    CadResult result = asm_result();
    CadAsmCatalog* candidate;
    size_t sourceIndex;
    if (!asmSources || asmSourceCount == 0 || !output) {
        asm_add_diagnostic(&result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_INVALID_ARGUMENT, 0, 0,
                           "ASM sources and catalog output are required");
        return result;
    }
    candidate = (CadAsmCatalog*)calloc(1, sizeof(*candidate));
    if (!candidate) {
        asm_add_diagnostic(&result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_OUT_OF_MEMORY, 0, 0,
                           "Could not allocate ASM catalog workspace");
        return result;
    }
    for (sourceIndex = 0; sourceIndex < asmSourceCount; ++sourceIndex) {
        AsmSourceView source;
        size_t lineIndex;
        const char* sourceName = asmSources[sourceIndex].name
                                     ? asmSources[sourceIndex].name : "";
        if (!make_source_view(&asmSources[sourceIndex], &source, &result)) {
            free(candidate);
            return result;
        }
        for (lineIndex = 0; lineIndex < source.lineCount; ++lineIndex) {
            char points[CAD_ASM_NAME_CAPACITY];
            char faces[CAD_ASM_NAME_CAPACITY];
            char label[CAD_ASM_NAME_CAPACITY];
            if (header_sections(source.lines[lineIndex].text,
                                points, sizeof(points), faces, sizeof(faces),
                                label, sizeof(label))) {
                char shapeName[CAD_ASM_NAME_CAPACITY];
                if (label[0]) copy_bounded(shapeName, sizeof(shapeName), label);
                else if (suffix_ci(points, "_P")) {
                    copy_bounded(shapeName, sizeof(shapeName), points);
                    shapeName[strlen(shapeName) - 2] = '\0';
                } else shapeName[0] = '\0';
                if (shapeName[0] &&
                    !catalog_has_alias(candidate, sourceIndex, shapeName,
                                       points) &&
                    !add_catalog_entry(candidate, sourceIndex, sourceName,
                                       &source.lines[lineIndex], shapeName,
                                       points, faces, &result)) {
                    free_source_view(&source);
                    free(candidate);
                    return result;
                }
            }
        }
        for (lineIndex = 0; lineIndex < source.lineCount; ++lineIndex) {
            char label[CAD_ASM_NAME_CAPACITY];
            char shapeName[CAD_ASM_NAME_CAPACITY];
            char faces[CAD_ASM_NAME_CAPACITY];
            size_t length;
            /* A ShapeHdr label may itself end in `_P` (for example OPCHR_P),
               and headers with zero topology are metadata-only entries.  Do
               not reinterpret either as a standalone point-section shape. */
            if (find_word_ci(source.lines[lineIndex].text,
                             "ShapeHdr", NULL, 0))
                continue;
            if (!first_word(source.lines[lineIndex].text,
                            label, sizeof(label)) || !suffix_ci(label, "_P"))
                continue;
            copy_bounded(shapeName, sizeof(shapeName), label);
            length = strlen(shapeName);
            shapeName[length - 2] = '\0';
            if (catalog_has_alias(candidate, sourceIndex, shapeName, label))
                continue;
            copy_bounded(faces, sizeof(faces), shapeName);
            if (strlen(faces) + 2 >= sizeof(faces)) {
                asm_add_diagnostic(&result, CAD_DIAGNOSTIC_ERROR,
                                   CAD_STATUS_INVALID_ARGUMENT,
                                   source.lines[lineIndex].byteOffset,
                                   source.lines[lineIndex].lineNumber,
                                   "ASM shape name is too long");
                free_source_view(&source);
                free(candidate);
                return result;
            }
            length = strlen(faces);
            faces[length] = '_';
            faces[length + 1] = 'F';
            faces[length + 2] = '\0';
            {
                size_t faceLine;
                int foundFaceSection = 0;
                for (faceLine = 0; faceLine < source.lineCount; ++faceLine) {
                    if (face_section_label(source.lines[faceLine].text,
                                           faces)) {
                        foundFaceSection = 1;
                        break;
                    }
                }
                if (!foundFaceSection) continue;
            }
            if (!add_catalog_entry(candidate, sourceIndex, sourceName,
                                   &source.lines[lineIndex], shapeName,
                                   label, faces, &result)) {
                free_source_view(&source);
                free(candidate);
                return result;
            }
        }
        free_source_view(&source);
    }
    qsort(candidate->shapes, candidate->shapeCount,
          sizeof(candidate->shapes[0]), compare_shape_info);
    for (sourceIndex = 1; sourceIndex < candidate->shapeCount; ++sourceIndex) {
        if (ascii_equal_ci(candidate->shapes[sourceIndex - 1].name,
                           candidate->shapes[sourceIndex].name)) {
            asm_add_diagnostic(&result, CAD_DIAGNOSTIC_WARNING,
                               CAD_STATUS_DUPLICATE_RECORD,
                               candidate->shapes[sourceIndex].byteOffset,
                               candidate->shapes[sourceIndex].lineNumber,
                               "Duplicate ASM shape name '%s'; deterministic first match is used",
                               candidate->shapes[sourceIndex].name);
        }
    }
    *output = *candidate;
    free(candidate);
    return result;
}

const CadAsmShapeInfo* CadImportAsm_FindShape(const CadAsmCatalog* catalog,
                                              const char* shapeName) {
    size_t low = 0;
    size_t high;
    if (!catalog || !shapeName || !shapeName[0]) return NULL;
    high = catalog->shapeCount;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        int comparison = ascii_compare_ci(catalog->shapes[middle].name,
                                          shapeName);
        if (comparison < 0) low = middle + 1;
        else high = middle;
    }
    if (low < catalog->shapeCount &&
        ascii_equal_ci(catalog->shapes[low].name, shapeName))
        return &catalog->shapes[low];
    return NULL;
}

static int constant_find(const AsmConstantTable* constants, const char* name) {
    size_t index;
    if (!constants || !name) return -1;
    for (index = 0; index < constants->count; ++index) {
        if (ascii_equal_ci(constants->entries[index].name, name))
            return (int)index;
    }
    return -1;
}

static int constant_get(const AsmConstantTable* constants, const char* name,
                        int* output) {
    int index = constant_find(constants, name);
    if (index < 0 || !output) return 0;
    *output = constants->entries[index].value;
    return 1;
}

static int constant_set(AsmConstantTable* constants, const char* name,
                        int value) {
    int index;
    if (!constants || !name || !name[0]) return 0;
    index = constant_find(constants, name);
    if (index >= 0) {
        constants->entries[index].value = value;
        return 1;
    }
    if (constants->count >= CAD_ASM_MAX_CONSTANTS) return 0;
    copy_bounded(constants->entries[constants->count].name,
                 sizeof(constants->entries[constants->count].name), name);
    constants->entries[constants->count].value = value;
    ++constants->count;
    return 1;
}

static int checked_value(long long value, AsmExpression* expression) {
    if (value < INT_MIN || value > INT_MAX) {
        expression->failed = 1;
        return 0;
    }
    return (int)value;
}

static int expression_sum(AsmExpression* expression);

static int expression_primary(AsmExpression* expression) {
    const char* start;
    int base = 10;
    long long value = 0;
    int digitCount = 0;
    char name[64];
    size_t nameLength = 0;
    expression->cursor = skip_space_const(expression->cursor);
    if (*expression->cursor == '(') {
        ++expression->cursor;
        value = expression_sum(expression);
        expression->cursor = skip_space_const(expression->cursor);
        if (*expression->cursor != ')') expression->failed = 1;
        else ++expression->cursor;
        return checked_value(value, expression);
    }
    if (*expression->cursor == '$') {
        base = 16;
        ++expression->cursor;
    } else if (*expression->cursor == '%') {
        base = 2;
        ++expression->cursor;
    } else if (expression->cursor[0] == '0' &&
               (expression->cursor[1] == 'x' ||
                expression->cursor[1] == 'X')) {
        base = 16;
        expression->cursor += 2;
    }
    start = expression->cursor;
    while (*expression->cursor) {
        int digit;
        unsigned char character = (unsigned char)*expression->cursor;
        if (character >= '0' && character <= '9') digit = character - '0';
        else if (character >= 'a' && character <= 'f')
            digit = character - 'a' + 10;
        else if (character >= 'A' && character <= 'F')
            digit = character - 'A' + 10;
        else break;
        if (digit >= base) break;
        if (value > (LLONG_MAX - digit) / base) {
            expression->failed = 1;
            return 0;
        }
        value = value * base + digit;
        ++expression->cursor;
        ++digitCount;
    }
    if (digitCount) return checked_value(value, expression);
    expression->cursor = start;
    if (isalpha((unsigned char)*expression->cursor) ||
        *expression->cursor == '_' || *expression->cursor == '.') {
        while (identifier_character((unsigned char)*expression->cursor)) {
            if (nameLength + 1 < sizeof(name))
                name[nameLength++] = *expression->cursor;
            ++expression->cursor;
        }
        name[nameLength] = '\0';
        {
            int constantValue;
            if (!constant_get(expression->constants, name, &constantValue)) {
                expression->failed = 1;
                return 0;
            }
            value = constantValue;
        }
        return (int)value;
    }
    expression->failed = 1;
    return 0;
}

static int expression_unary(AsmExpression* expression) {
    int sign = 1;
    long long value;
    expression->cursor = skip_space_const(expression->cursor);
    while (*expression->cursor == '+' || *expression->cursor == '-') {
        if (*expression->cursor == '-') sign = -sign;
        ++expression->cursor;
        expression->cursor = skip_space_const(expression->cursor);
    }
    value = expression_primary(expression);
    return checked_value(value * sign, expression);
}

static int expression_product(AsmExpression* expression) {
    long long value = expression_unary(expression);
    while (!expression->failed) {
        char operation;
        int right;
        expression->cursor = skip_space_const(expression->cursor);
        operation = *expression->cursor;
        if (operation != '*' && operation != '/' && operation != '%') break;
        ++expression->cursor;
        right = expression_unary(expression);
        if (expression->failed) break;
        if ((operation == '/' || operation == '%') && right == 0) {
            expression->failed = 1;
            break;
        }
        if (operation == '*') value *= right;
        else if (operation == '/') value /= right;
        else value %= right;
        value = checked_value(value, expression);
    }
    return checked_value(value, expression);
}

static int expression_sum(AsmExpression* expression) {
    long long value = expression_product(expression);
    while (!expression->failed) {
        char operation;
        int right;
        expression->cursor = skip_space_const(expression->cursor);
        operation = *expression->cursor;
        if (operation != '+' && operation != '-') break;
        ++expression->cursor;
        right = expression_product(expression);
        if (expression->failed) break;
        if (operation == '+') value += right;
        else value -= right;
        value = checked_value(value, expression);
    }
    return checked_value(value, expression);
}

static int parse_expression_prefix(const char* text,
                                   const AsmConstantTable* constants,
                                   int* output, const char** end) {
    AsmExpression expression;
    int value;
    if (!text || !output) return 0;
    expression.cursor = text;
    expression.constants = constants;
    expression.failed = 0;
    value = expression_sum(&expression);
    expression.cursor = skip_space_const(expression.cursor);
    if (expression.failed) return 0;
    *output = value;
    if (end) *end = expression.cursor;
    return 1;
}

static int parse_constant_definition(const char* line,
                                     AsmConstantTable* constants,
                                     int* tableFull) {
    const char* cursor = skip_space_const(line);
    const char* valueStart;
    char name[64];
    size_t length = 0;
    int value;
    if (!cursor || (!isalpha((unsigned char)*cursor) &&
                    *cursor != '_' && *cursor != '.')) return 0;
    while (identifier_character((unsigned char)*cursor)) {
        if (length + 1 < sizeof(name)) name[length++] = *cursor;
        ++cursor;
    }
    name[length] = '\0';
    cursor = skip_space_const(cursor);
    if (*cursor == '=') ++cursor;
    else if (ascii_starts_ci(cursor, "equ") &&
             !identifier_character((unsigned char)cursor[3])) cursor += 3;
    else return 0;
    valueStart = skip_space_const(cursor);
    if (!parse_expression_prefix(valueStart, constants, &value, &cursor))
        return -1;
    cursor = skip_space_const(cursor);
    if (*cursor) return -1;
    if (!constant_set(constants, name, value)) {
        if (tableFull) *tableFull = 1;
        return -1;
    }
    return 1;
}

static int load_constant_view(const AsmSourceView* source,
                              size_t beginLine, size_t endLine,
                              AsmConstantTable* constants,
                              CadResult* result) {
    size_t pass;
    size_t index;
    size_t previousCount = constants->count;
    int tableFull = 0;
    if (endLine > source->lineCount) endLine = source->lineCount;
    for (pass = 0; pass < 16; ++pass) {
        int progress = 0;
        for (index = beginLine; index < endLine; ++index) {
            size_t before = constants->count;
            int status = parse_constant_definition(source->lines[index].text,
                                                   constants, &tableFull);
            if (status > 0 || constants->count != before) progress = 1;
        }
        if (tableFull) {
            asm_add_diagnostic(result, CAD_DIAGNOSTIC_ERROR,
                               CAD_STATUS_INDEX_OUT_OF_RANGE, 0, 0,
                               "ASM constants exceed the %u-entry limit",
                               (unsigned)CAD_ASM_MAX_CONSTANTS);
            return 0;
        }
        if (!progress || constants->count == previousCount) break;
        previousCount = constants->count;
    }
    return 1;
}

static int load_constant_sources(const CadAsmTextSource* sources,
                                 size_t sourceCount,
                                 AsmConstantTable* constants,
                                 CadResult* result) {
    size_t index;
    if (!sources && sourceCount) {
        asm_add_diagnostic(result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_INVALID_ARGUMENT, 0, 0,
                           "Constant source count requires source buffers");
        return 0;
    }
    for (index = 0; index < sourceCount; ++index) {
        AsmSourceView source;
        if (!make_source_view(&sources[index], &source, result)) return 0;
        if (!load_constant_view(&source, 0, source.lineCount,
                                constants, result)) {
            free_source_view(&source);
            return 0;
        }
        free_source_view(&source);
    }
    return 1;
}

static int line_has_word(const char* line, const char* word) {
    return find_word_ci(line, word, NULL, 0) != NULL;
}

static int line_label_equals(const char* line, const char* label) {
    char word[CAD_ASM_NAME_CAPACITY];
    return first_word(line, word, sizeof(word)) && ascii_equal_ci(word, label);
}

static int face_section_label(const char* line, const char* base) {
    char word[CAD_ASM_NAME_CAPACITY];
    size_t baseLength;
    const char* suffix;
    if (!first_word(line, word, sizeof(word))) return 0;
    baseLength = strlen(base);
    if (!ascii_starts_ci(word, base)) return 0;
    suffix = word + baseLength;
    if (!*suffix) return 1;
    while (*suffix) {
        if (!isdigit((unsigned char)*suffix)) return 0;
        ++suffix;
    }
    return 1;
}

static const char* find_directive(const char* line, const char* directive) {
    return find_word_ci(line, directive, NULL, 0);
}

static int parse_csv_integers(const char* text,
                              const AsmConstantTable* constants,
                              int* values, size_t valueCount) {
    size_t index;
    const char* cursor = text;
    for (index = 0; index < valueCount; ++index) {
        if (!parse_expression_prefix(cursor, constants, &values[index],
                                     &cursor)) return 0;
        cursor = skip_space_const(cursor);
        if (index + 1 < valueCount) {
            if (*cursor != ',') return 0;
            ++cursor;
        }
    }
    cursor = skip_space_const(cursor);
    return *cursor == '\0';
}

static int append_source_vertex(AsmVertex* vertices, size_t* count,
                                int x, int y, int z, int mirrored,
                                int invertY, const AsmLine* line,
                                CadResult* result) {
    size_t required = mirrored ? 2 : 1;
    if (*count + required > CAD_ASM_MAX_SOURCE_POINTS) {
        asm_add_diagnostic(result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_INDEX_OUT_OF_RANGE,
                           line->byteOffset, line->lineNumber,
                           "ASM source points exceed the %u-point limit",
                           (unsigned)CAD_ASM_MAX_SOURCE_POINTS);
        return 0;
    }
    vertices[*count].x = x;
    vertices[*count].y = invertY ? -y : y;
    vertices[*count].z = z;
    ++*count;
    if (mirrored) {
        vertices[*count].x = -x;
        vertices[*count].y = invertY ? -y : y;
        vertices[*count].z = z;
        ++*count;
    }
    return 1;
}

static int parse_points(const AsmSourceView* source, size_t pointStart,
                        const AsmConstantTable* constants, int invertY,
                        AsmVertex* vertices, size_t* vertexCount,
                        size_t* endLine, CadResult* result) {
    size_t lineIndex;
    int mirrored = 0;
    int foundEnd = 0;
    for (lineIndex = pointStart + 1; lineIndex < source->lineCount;
         ++lineIndex) {
        const AsmLine* line = &source->lines[lineIndex];
        const char* directive;
        int values[3];
        if (line_has_word(line->text, "EndPoints")) {
            foundEnd = 1;
            ++lineIndex;
            break;
        }
        if (find_directive(line->text, "PointsXb") ||
            find_directive(line->text, "PointsXw")) {
            mirrored = 1;
            continue;
        }
        if (find_directive(line->text, "Pointsb") ||
            find_directive(line->text, "Pointsw")) {
            mirrored = 0;
            continue;
        }
        directive = find_directive(line->text, "pbd2");
        if (!directive) directive = find_directive(line->text, "pwd2");
        if (directive) {
            if (!parse_csv_integers(directive + 4, constants, values, 3)) {
                asm_add_diagnostic(result, CAD_DIAGNOSTIC_ERROR,
                                   CAD_STATUS_INVALID_NUMBER,
                                   line->byteOffset, line->lineNumber,
                                   "Malformed or unresolved half-size ASM point");
                return 0;
            }
            values[0] /= 2;
            values[1] /= 2;
            values[2] /= 2;
            if (!append_source_vertex(vertices, vertexCount,
                                      values[0], values[1], values[2],
                                      mirrored, invertY, line, result)) return 0;
            continue;
        }
        directive = find_directive(line->text, "pb");
        if (!directive) directive = find_directive(line->text, "pw");
        if (directive) {
            if (!parse_csv_integers(directive + 2, constants, values, 3)) {
                asm_add_diagnostic(result, CAD_DIAGNOSTIC_ERROR,
                                   CAD_STATUS_INVALID_NUMBER,
                                   line->byteOffset, line->lineNumber,
                                   "Malformed or unresolved ASM point");
                return 0;
            }
            if (!append_source_vertex(vertices, vertexCount,
                                      values[0], values[1], values[2],
                                      mirrored, invertY, line, result)) return 0;
        }
    }
    if (!foundEnd) {
        asm_add_diagnostic(result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_TRUNCATED_RECORD,
                           source->lines[pointStart].byteOffset,
                           source->lines[pointStart].lineNumber,
                           "ASM point section '%s' has no EndPoints",
                           source->lines[pointStart].text);
        return 0;
    }
    if (*vertexCount == 0) {
        asm_add_diagnostic(result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_INVALID_TOPOLOGY,
                           source->lines[pointStart].byteOffset,
                           source->lines[pointStart].lineNumber,
                           "ASM point section contains no supported points");
        return 0;
    }
    if (endLine) *endLine = lineIndex;
    return 1;
}

static int append_face(CadFileData* data, const AsmVertex* vertices,
                       size_t vertexCount, const int* arguments,
                       int facePointCount, const AsmLine* line,
                       CadResult* result) {
    CadPolygon* polygon;
    int polygonIndex;
    int pointIndex;
    int ordinal;
    int color = arguments[0];
    int normalX = arguments[2];
    int normalY = arguments[3];
    int normalZ = arguments[4];
    unsigned side = 0;
    if (color < 0 || color > 255) {
        asm_add_diagnostic(result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_INDEX_OUT_OF_RANGE,
                           line->byteOffset, line->lineNumber,
                           "ASM face color %d is outside 0..255", color);
        return 0;
    }
    if (data->polygonCount >= CAD_MAX_POLYGONS ||
        data->pointCount + facePointCount > CAD_MAX_POINTS) {
        asm_add_diagnostic(result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_INDEX_OUT_OF_RANGE,
                           line->byteOffset, line->lineNumber,
                           "ASM shape exceeds native polygon/point capacity");
        return 0;
    }
    for (ordinal = 0; ordinal < facePointCount; ++ordinal) {
        int sourceIndex = arguments[5 + ordinal];
        if (sourceIndex < 0 || (size_t)sourceIndex >= vertexCount) {
            asm_add_diagnostic(result, CAD_DIAGNOSTIC_ERROR,
                               CAD_STATUS_INDEX_OUT_OF_RANGE,
                               line->byteOffset, line->lineNumber,
                               "ASM face vertex %d is outside 0..%u",
                               sourceIndex,
                               vertexCount ? (unsigned)(vertexCount - 1) : 0);
            return 0;
        }
    }
    polygonIndex = data->polygonCount++;
    pointIndex = data->pointCount;
    polygon = &data->polygons[polygonIndex];
    polygon->flags = 1;
    polygon->selectFlag = 0;
    polygon->nextPolygon = -1;
    polygon->firstPoint = (int16_t)pointIndex;
    polygon->animation = -1;
    polygon->both = -1;
    if (normalY < 0) side |= 1u;
    if (normalZ >= 0) side |= 2u;
    if (normalX < 0) side |= 4u;
    polygon->side = (uint8_t)side;
    polygon->color = (uint8_t)color;
    polygon->npoints = (uint8_t)facePointCount;
    if (polygonIndex > 0)
        data->polygons[polygonIndex - 1].nextPolygon = (int16_t)polygonIndex;
    for (ordinal = 0; ordinal < facePointCount; ++ordinal) {
        int sourceIndex = arguments[5 + ordinal];
        CadPoint* point = &data->points[pointIndex + ordinal];
        point->flags = 2;
        point->selectFlag = 0;
        point->nextPoint = ordinal + 1 < facePointCount
                               ? (int16_t)(pointIndex + ordinal + 1) : -1;
        point->pointx = vertices[sourceIndex].x;
        point->pointy = vertices[sourceIndex].y;
        point->pointz = vertices[sourceIndex].z;
    }
    data->pointCount += facePointCount;
    return 1;
}

static int parse_face_directive(const char* line, const char** arguments,
                                int* facePointCount) {
    const char* cursor = line;
    const char* start = NULL;
    char word[32];
    while (next_word(&cursor, word, sizeof(word), &start)) {
        const char* digits;
        long count;
        char* end = NULL;
        if (!ascii_starts_ci(word, "Face")) {
            cursor = skip_space_const(cursor);
            if (*cursor == ',') ++cursor;
            continue;
        }
        digits = word + 4;
        /* Section declarations use the recovered `Faces` macro and labels
           commonly contain words such as `Custom_F1 Faces`.  Only a Face
           token followed by a decimal count is an actual face directive. */
        if (!isdigit((unsigned char)*digits)) {
            cursor = skip_space_const(cursor);
            if (*cursor == ',') ++cursor;
            continue;
        }
        count = strtol(digits, &end, 10);
        if (!end || *end || count < CAD_MIN_FACE_POINTS ||
            count > CAD_MAX_FACE_POINTS) return -1;
        *facePointCount = (int)count;
        *arguments = start + strlen(word);
        return 1;
    }
    return 0;
}

static int parse_faces(const AsmSourceView* source, size_t faceStart,
                       const AsmConstantTable* constants,
                       const AsmVertex* vertices, size_t vertexCount,
                       CadFileData* output, CadResult* result) {
    size_t lineIndex;
    int foundEnd = 0;
    for (lineIndex = faceStart; lineIndex < source->lineCount; ++lineIndex) {
        const AsmLine* line = &source->lines[lineIndex];
        const char* argumentText = NULL;
        int facePointCount = 0;
        int status;
        int arguments[5 + CAD_MAX_FACE_POINTS];
        if (line_has_word(line->text, "EndShape")) {
            foundEnd = 1;
            break;
        }
        status = parse_face_directive(line->text, &argumentText,
                                      &facePointCount);
        if (status < 0) {
            asm_add_diagnostic(result, CAD_DIAGNOSTIC_ERROR,
                               CAD_STATUS_INDEX_OUT_OF_RANGE,
                               line->byteOffset, line->lineNumber,
                               "ASM faces must contain 2..16 points");
            return 0;
        }
        if (status == 0) continue;
        if (!parse_csv_integers(argumentText, constants, arguments,
                                (size_t)(5 + facePointCount))) {
            asm_add_diagnostic(result, CAD_DIAGNOSTIC_ERROR,
                               CAD_STATUS_INVALID_NUMBER,
                               line->byteOffset, line->lineNumber,
                               "Malformed or unresolved Face%d directive",
                               facePointCount);
            return 0;
        }
        if (!append_face(output, vertices, vertexCount, arguments,
                         facePointCount, line, result)) return 0;
    }
    if (!foundEnd) {
        asm_add_diagnostic(result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_TRUNCATED_RECORD,
                           source->lines[faceStart].byteOffset,
                           source->lines[faceStart].lineNumber,
                           "ASM face section has no EndShape");
        return 0;
    }
    if (output->polygonCount == 0) {
        asm_add_diagnostic(result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_INVALID_TOPOLOGY,
                           source->lines[faceStart].byteOffset,
                           source->lines[faceStart].lineNumber,
                           "ASM shape contains no supported Face2..Face16 directives");
        return 0;
    }
    output->objects[0].flags = 1;
    output->objects[0].selectFlag = 0;
    output->objects[0].parentObject = -1;
    output->objects[0].nextBrother = -1;
    output->objects[0].childObject = -1;
    output->objects[0].firstPolygon = 0;
    output->objectCount = 1;
    return 1;
}

static void merge_diagnostics(CadResult* destination,
                              const CadResult* source) {
    size_t index;
    if (!destination || !source) return;
    for (index = 0; index < source->diagnosticCount; ++index) {
        const CadDiagnostic* diagnostic = &source->diagnostics[index];
        asm_add_diagnostic(destination, diagnostic->severity,
                           diagnostic->code, diagnostic->byteOffset,
                           diagnostic->recordIndex < 0
                               ? 0u : (size_t)diagnostic->recordIndex,
                           "%s", diagnostic->message);
    }
    if (source->status != CAD_STATUS_OK && destination->status == CAD_STATUS_OK)
        destination->status = source->status;
}

CadResult CadImportAsm_DecodeCatalogShape(
    const CadAsmTextSource* asmSources, size_t asmSourceCount,
    const CadAsmTextSource* constantSources, size_t constantSourceCount,
    const CadAsmShapeInfo* shape, const CadAsmImportOptions* options,
    CadFileData* output, CadAsmImportInfo* info) {
    CadResult result = asm_result();
    CadResult validation;
    CadAsmImportOptions defaults = CadImportAsm_DefaultOptions();
    AsmSourceView source;
    AsmConstantTable constants;
    AsmVertex* vertices = NULL;
    CadFileData* candidate = NULL;
    CadAsmImportInfo candidateInfo;
    size_t pointStart = SIZE_MAX;
    size_t pointEnd = SIZE_MAX;
    size_t faceStart = SIZE_MAX;
    size_t localStart = 0;
    size_t lineIndex;
    int macroGeneratedPoints = 0;
    if (!asmSources || asmSourceCount == 0 || !shape || !output ||
        shape->sourceIndex >= asmSourceCount) {
        asm_add_diagnostic(&result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_INVALID_ARGUMENT, 0, 0,
                           "Valid ASM sources, shape entry, and output are required");
        return result;
    }
    if (!options) options = &defaults;
    memset(&constants, 0, sizeof(constants));
    memset(&candidateInfo, 0, sizeof(candidateInfo));
    if (!load_constant_sources(constantSources, constantSourceCount,
                               &constants, &result)) return result;
    if (!make_source_view(&asmSources[shape->sourceIndex], &source, &result))
        return result;
    for (lineIndex = 0; lineIndex < source.lineCount; ++lineIndex) {
        if (pointStart == SIZE_MAX &&
            line_label_equals(source.lines[lineIndex].text,
                              shape->pointSection)) pointStart = lineIndex;
        if (faceStart == SIZE_MAX &&
            face_section_label(source.lines[lineIndex].text,
                               shape->faceSection)) faceStart = lineIndex;
    }
    if (pointStart == SIZE_MAX) {
        asm_add_diagnostic(&result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_UNRECOGNIZED_FORMAT, shape->byteOffset,
                           shape->lineNumber,
                           "ASM point section '%s' was not found in '%s'",
                           shape->pointSection,
                           asmSources[shape->sourceIndex].name
                               ? asmSources[shape->sourceIndex].name : "(unnamed)");
        free_source_view(&source);
        return result;
    }
    if (faceStart == SIZE_MAX) {
        asm_add_diagnostic(&result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_UNRECOGNIZED_FORMAT, shape->byteOffset,
                           shape->lineNumber,
                           "ASM face section '%s' was not found in '%s'",
                           shape->faceSection,
                           asmSources[shape->sourceIndex].name
                               ? asmSources[shape->sourceIndex].name : "(unnamed)");
        free_source_view(&source);
        return result;
    }
    for (lineIndex = pointStart + 1;
         lineIndex < source.lineCount && lineIndex < faceStart; ++lineIndex) {
        if (line_has_word(source.lines[lineIndex].text, "DataHdr") ||
            line_has_word(source.lines[lineIndex].text, "JumpTab"))
            macroGeneratedPoints = 1;
    }
    /* Keep local definitions shape-scoped.  Start after the preceding shape
       boundary; this handles constants immediately before ShapeHdr without
       allowing a later shape's assignments to leak into this preview. */
    for (lineIndex = pointStart; lineIndex > 0; --lineIndex) {
        if (line_has_word(source.lines[lineIndex - 1].text, "EndShape")) {
            localStart = lineIndex;
            break;
        }
    }
    if (!load_constant_view(&source, localStart, pointStart,
                            &constants, &result)) {
        free_source_view(&source);
        return result;
    }
    vertices = (AsmVertex*)calloc(CAD_ASM_MAX_SOURCE_POINTS,
                                  sizeof(*vertices));
    candidate = (CadFileData*)malloc(sizeof(*candidate));
    if (!vertices || !candidate) {
        asm_add_diagnostic(&result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_OUT_OF_MEMORY, 0, 0,
                           "Could not allocate ASM import workspace");
        free(vertices);
        free(candidate);
        free_source_view(&source);
        return result;
    }
    CadFile_Init(candidate);
    if (!parse_points(&source, pointStart, &constants, options->invertY != 0,
                      vertices, &candidateInfo.sourcePointCount, &pointEnd,
                      &result) ||
        !parse_faces(&source, faceStart, &constants, vertices,
                     candidateInfo.sourcePointCount, candidate, &result)) {
        if (macroGeneratedPoints &&
            (result.status == CAD_STATUS_INDEX_OUT_OF_RANGE ||
             result.status == CAD_STATUS_INVALID_NUMBER ||
             result.status == CAD_STATUS_INVALID_TOPOLOGY)) {
            result.status = CAD_STATUS_UNRECOGNIZED_FORMAT;
            asm_add_diagnostic(
                &result, CAD_DIAGNOSTIC_ERROR,
                CAD_STATUS_UNRECOGNIZED_FORMAT,
                source.lines[pointStart].byteOffset,
                source.lines[pointStart].lineNumber,
                "This game-runtime animated ASM point stream cannot be reduced to a static preview");
        }
        free(vertices);
        free(candidate);
        free_source_view(&source);
        return result;
    }
    (void)pointEnd;
    validation = CadCodec_Validate(candidate);
    if (!CadResult_IsSuccess(&validation)) {
        merge_diagnostics(&result, &validation);
        free(vertices);
        free(candidate);
        free_source_view(&source);
        return result;
    }
    merge_diagnostics(&result, &validation);
    candidateInfo.shape = *shape;
    candidateInfo.polygonCount = (size_t)candidate->polygonCount;
    candidateInfo.generatedPointCount = (size_t)candidate->pointCount;
    *output = *candidate;
    if (info) *info = candidateInfo;
    result.bytesConsumed = asmSources[shape->sourceIndex].size;
    free(vertices);
    free(candidate);
    free_source_view(&source);
    return result;
}

CadResult CadImportAsm_DecodeShape(
    const CadAsmTextSource* asmSources, size_t asmSourceCount,
    const CadAsmTextSource* constantSources, size_t constantSourceCount,
    const char* shapeName, const CadAsmImportOptions* options,
    CadFileData* output, CadAsmImportInfo* info) {
    CadResult catalogResult;
    CadResult result;
    CadAsmCatalog* catalog;
    const CadAsmShapeInfo* shape;
    if (!shapeName || !shapeName[0] || !output) {
        result = asm_result();
        asm_add_diagnostic(&result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_INVALID_ARGUMENT, 0, 0,
                           "ASM shape name and output are required");
        return result;
    }
    catalog = (CadAsmCatalog*)malloc(sizeof(*catalog));
    if (!catalog) {
        result = asm_result();
        asm_add_diagnostic(&result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_OUT_OF_MEMORY, 0, 0,
                           "Could not allocate ASM catalog workspace");
        return result;
    }
    catalogResult = CadImportAsm_BuildCatalog(asmSources, asmSourceCount,
                                              catalog);
    if (!CadResult_IsSuccess(&catalogResult)) {
        free(catalog);
        return catalogResult;
    }
    shape = CadImportAsm_FindShape(catalog, shapeName);
    if (!shape) {
        result = catalogResult;
        asm_add_diagnostic(&result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_UNRECOGNIZED_FORMAT, 0, 0,
                           "ASM shape '%s' was not found", shapeName);
        free(catalog);
        return result;
    }
    result = CadImportAsm_DecodeCatalogShape(
        asmSources, asmSourceCount, constantSources, constantSourceCount,
        shape, options, output, info);
    if (CadResult_IsSuccess(&result)) {
        /* Catalog duplicate warnings remain useful to search/replace callers. */
        merge_diagnostics(&result, &catalogResult);
    }
    free(catalog);
    return result;
}

CadResult CadImportAsm_DecodeShapeToCore(
    const CadAsmTextSource* asmSources, size_t asmSourceCount,
    const CadAsmTextSource* constantSources, size_t constantSourceCount,
    const char* shapeName, const CadAsmImportOptions* options,
    CadCore* output, CadAsmImportInfo* info) {
    CadResult result;
    CadFileData* decoded;
    if (!output) {
        result = asm_result();
        asm_add_diagnostic(&result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_INVALID_ARGUMENT, 0, 0,
                           "CadCore output is required");
        return result;
    }
    decoded = (CadFileData*)malloc(sizeof(*decoded));
    if (!decoded) {
        result = asm_result();
        asm_add_diagnostic(&result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_OUT_OF_MEMORY, 0, 0,
                           "Could not allocate ASM preview workspace");
        return result;
    }
    result = CadImportAsm_DecodeShape(
        asmSources, asmSourceCount, constantSources, constantSourceCount,
        shapeName, options, decoded, info);
    if (CadResult_IsSuccess(&result)) {
        CadCore_Clear(output);
        output->data = *decoded;
        CadCore_RebuildDerivedState(output);
        output->isDirty = 1;
    }
    free(decoded);
    return result;
}
