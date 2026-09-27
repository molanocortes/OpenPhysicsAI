/* coretest.c - unit tests for the dependency-free core: JSON, JSON Schema, units, SHA-256, base64, paths.
 *   make build/coretest && ./build/coretest */
#include "../src/core/base64.h"
#include "../src/core/errors.h"
#include "../src/core/jschema.h"
#include "../src/core/json.h"
#include "../src/core/paths.h"
#include "../src/core/sha256.h"
#include "../src/core/units.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fail, g_pass;

#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        if (cond) {                                                                                                   \
            g_pass++;                                                                                                 \
        } else {                                                                                                      \
            g_fail++;                                                                                                 \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                                                             \
            printf(__VA_ARGS__);                                                                                      \
            printf("\n");                                                                                             \
        }                                                                                                             \
    } while (0)

static bool near(double a, double b, double rel) { return fabs(a - b) <= rel * fmax(fabs(a), fabs(b)) + 1e-300; }

static void test_json_valid(void) {
    printf("== JSON: valid documents\n");
    const char *doc = "{\"a\": 1, \"b\": [true, false, null], \"c\": \"x\\u00e9\\ud83d\\ude00\\n\", \"d\": -0.5e2,"
                      " \"e\": {\"f\": []}}";
    JsonError err;
    JsonValue *v = json_parse(doc, strlen(doc), NULL, &err);
    CHECK(v != NULL, "parse failed: %s", err.message);
    if (!v) return;
    CHECK(json_get_num(v, "a", 0) == 1, "a");
    CHECK(json_len(json_get(v, "b")) == 3, "b length");
    CHECK(json_at(json_get(v, "b"), 2)->type == JSON_NULL, "null element");
    CHECK(strcmp(json_get_str(v, "c", ""), "x\xC3\xA9\xF0\x9F\x98\x80\n") == 0, "unicode decode");
    CHECK(json_get_num(v, "d", 0) == -50, "exponent number");
    CHECK(json_get(json_get(v, "e"), "f")->type == JSON_ARRAY, "nested");
    char *s = json_dump(v, 0, NULL, NULL);
    JsonValue *w = json_parse(s, strlen(s), NULL, &err);
    CHECK(w && json_equal(v, w), "round trip: %s", s);
    CHECK(strchr(s, '\n') == NULL, "compact dump has no raw newline");
    free(s);
    json_free(w);
    json_free(v);

    static const double nums[] = {0.1, 1e-300, 123456789012345678.0, 3.141592653589793, -2.5, 1e21, 5e-324, 9007199254740993.0};
    for (size_t i = 0; i < sizeof nums / sizeof nums[0]; i++) {
        JsonValue *n = json_number(nums[i]);
        char *t = json_dump(n, 0, NULL, NULL);
        JsonValue *back = json_parse(t, strlen(t), NULL, NULL);
        CHECK(back && back->u.number == nums[i], "number round trip %.17g -> %s", nums[i], t);
        free(t);
        json_free(n);
        json_free(back);
    }
    bool nonfinite = false;
    JsonValue *nanv = json_number(NAN);
    char *t = json_dump(nanv, 0, NULL, &nonfinite);
    CHECK(t && strcmp(t, "null") == 0 && nonfinite, "NaN written as null and reported");
    free(t);
    json_free(nanv);

    JsonValue *o = json_object();
    json_set_string(o, "z", "ctl\x01\"q\"\\");
    json_set_number(o, "a", 2);
    json_set_string(o, "bad", "ok\xFFok");
    json_set_number(o, "a", 3); /* replace keeps position */
    t = json_dump(o, JSON_SORTED, NULL, NULL);
    CHECK(t && strcmp(t, "{\"a\":3,\"bad\":\"ok\xEF\xBF\xBDok\",\"z\":\"ctl\\u0001\\\"q\\\"\\\\\"}") == 0, "sorted dump / escaping: %s", t);
    free(t);
    CHECK(json_len(o) == 3 && strcmp(json_key_at(o, 0), "z") == 0, "insertion order kept");
    JsonValue *taken = json_take(o, "z");
    CHECK(taken && json_len(o) == 2 && !json_get(o, "z"), "take");
    json_free(taken);
    json_free(o);
}

static void test_json_invalid(void) {
    printf("== JSON: rejected documents\n");
    static const char *bad[] = {
        "{\"a\":1,}", "[1,]", "{\"a\" 1}", "01", "1.", ".5", "-", "1e", "\"\x01\"", "\"\\ud800\"", "\"\\udc00x\"",
        "\"\\u0000\"", "{\"a\":1,\"a\":2}", "nul", "tru", "\xEF\xBB\xBF{}", "{} x", "\"\xFF\"", "\"\xC0\x80\"",
        "\"\xED\xA0\x80\"", "1e999", "NaN", "Infinity", "'x'", "[1 2]", "{\"a\":}", "\"\\x\"", "\"abc", "", " ",
        "+1", "0x10", "[\"a\"\n,]", "{\"a\":1}}", "\"\\u12\"", "-01", "1e+", "[-]",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        JsonError err;
        JsonValue *v = json_parse(bad[i], strlen(bad[i]), NULL, &err);
        CHECK(v == NULL && err.message[0], "should reject #%zu", i);
        json_free(v);
    }
    /* nesting limit */
    char deep[200];
    memset(deep, '[', 70);
    memset(deep + 70, ']', 70);
    deep[140] = 0;
    JsonError err;
    JsonValue *v = json_parse(deep, 140, NULL, &err);
    CHECK(v == NULL && strstr(err.message, "nesting"), "depth limit: %s", err.message);
    JsonLimits lim = {100, 4, 2, 3};
    v = json_parse("\"abcde\"", 7, &lim, &err);
    CHECK(!v && strstr(err.message, "longer"), "string limit");
    v = json_parse("{\"a\":1,\"b\":2,\"c\":3}", 19, &lim, &err);
    CHECK(!v && strstr(err.message, "keys"), "member limit");
    v = json_parse("[1,2,3,4]", 9, &lim, &err);
    CHECK(!v && strstr(err.message, "array"), "items limit");
    v = json_parse("{\n  \"a\": tru\n}", 14, NULL, &err);
    CHECK(!v && err.line == 2, "line number reported (%d:%d %s)", err.line, err.column, err.message);
}

static void test_sha_base64(void) {
    printf("== SHA-256 and base64 known answers\n");
    char hex[65];
    sha256_hex_of("", 0, hex);
    CHECK(!strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), "sha256('') %s", hex);
    sha256_hex_of("abc", 3, hex);
    CHECK(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "sha256(abc) %s", hex);
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256_hex_of(m, strlen(m), hex);
    CHECK(!strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"), "sha256(448 bits) %s", hex);
    Sha256 s;
    sha256_init(&s);
    char block[1000];
    memset(block, 'a', sizeof block);
    for (int i = 0; i < 1000; i++) sha256_update(&s, block, sizeof block);
    unsigned char d[32];
    sha256_final(&s, d);
    sha256_hex(d, hex);
    CHECK(!strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"), "sha256(1M a) %s", hex);

    static const char *plain[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    static const char *enc[] = {"", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
    for (int i = 0; i < 7; i++) {
        size_t n;
        char *e = base64_encode((const unsigned char *)plain[i], strlen(plain[i]), &n);
        CHECK(e && !strcmp(e, enc[i]), "base64(%s) = %s", plain[i], e ? e : "(null)");
        unsigned char *back = base64_decode(enc[i], strlen(enc[i]), &n);
        CHECK(back && n == strlen(plain[i]) && !memcmp(back, plain[i], n), "decode %s", enc[i]);
        free(e);
        free(back);
    }
    static const char *badb64[] = {"Zg=", "Z===", "Zm9v!A==", "=Zm9", "Zg==Zg=="};
    for (int i = 0; i < 5; i++) {
        size_t n;
        unsigned char *b = base64_decode(badb64[i], strlen(badb64[i]), &n);
        CHECK(b == NULL, "reject base64 %s", badb64[i]);
        free(b);
    }
}

static void test_units(void) {
    printf("== units\n");
    struct {
        const char *text;
        Dimension dim;
        double si;
    } ok[] = {
        {"0.2 mm", DIM_LENGTH, 2e-4},
        {"0.2mm", DIM_LENGTH, 2e-4},
        {"60 degC", DIM_TEMPERATURE, 333.15},
        {"60 \xC2\xB0" "C", DIM_TEMPERATURE, 333.15},
        {"140 F", DIM_TEMPERATURE, 333.15},
        {"333.15 K", DIM_TEMPERATURE, 333.15},
        {"10 degC", DIM_TEMPERATURE_DIFFERENCE, 10},
        {"18 degF", DIM_TEMPERATURE_DIFFERENCE, 10},
        {"3.5 GPa", DIM_PRESSURE, 3.5e9},
        {"250 N/mm^2", DIM_PRESSURE, 250e6},
        {"10 psi", DIM_PRESSURE, 68947.57293168361},
        {"0.13 W/(m*K)", DIM_THERMAL_CONDUCTIVITY, 0.13},
        {"0.13 W/m/K", DIM_THERMAL_CONDUCTIVITY, 0.13},
        {"0.13 W/m*K", DIM_THERMAL_CONDUCTIVITY, 0.13},
        {"0.13 W/(m\xC2\xB7K)", DIM_THERMAL_CONDUCTIVITY, 0.13},
        {"1800 J/kg/K", DIM_SPECIFIC_HEAT, 1800},
        {"1.8 kJ/(kg*K)", DIM_SPECIFIC_HEAT, 1800},
        {"25 W/m^2/K", DIM_HEAT_TRANSFER_COEFFICIENT, 25},
        {"25 W/m\xC2\xB2/K", DIM_HEAT_TRANSFER_COEFFICIENT, 25},
        {"25 W m^-2 K^-1", DIM_HEAT_TRANSFER_COEFFICIENT, 25},
        {"68e-6 1/K", DIM_EXPANSION_COEFFICIENT, 68e-6},
        {"68e-6 1/degC", DIM_EXPANSION_COEFFICIENT, 68e-6},
        {"200 N", DIM_FORCE, 200},
        {"1.5 kN", DIM_FORCE, 1500},
        {"45 lbf", DIM_FORCE, 45 * 4.4482216152605},
        {"0.5 in", DIM_LENGTH, 0.0127},
        {"45 deg", DIM_ANGLE, M_PI / 4},
        {"60 mm/s", DIM_SPEED, 0.06},
        {"1 m/min", DIM_SPEED, 1.0 / 60},
        {"7850 kg/m^3", DIM_DENSITY, 7850},
        {"8 g/cm^3", DIM_DENSITY, 8000},
        {"8 g/cm\xC2\xB3", DIM_DENSITY, 8000},
        {"280 W", DIM_POWER, 280},
        {"1 N m", DIM_ENERGY, 1},
        {"2 min", DIM_TIME, 120},
        {"350 ms", DIM_TIME, 0.35},
        {"50 um", DIM_LENGTH, 50e-6},
        {"50 \xC2\xB5m", DIM_LENGTH, 50e-6},
        {"290 kJ/kg", DIM_SPECIFIC_ENERGY, 290e3},
        {"-5e-3 in", DIM_LENGTH, -5e-3 * 0.0254},
        {"100 %", DIM_DIMENSIONLESS, 1.0},
        {"0.35", DIM_DIMENSIONLESS, 0.35},
        {"1e6 W/m^3", DIM_VOLUMETRIC_POWER, 1e6},
    };
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++) {
        double si = NAN;
        char err[200];
        bool r = quantity_parse(ok[i].text, ok[i].dim, &si, err, sizeof err);
        CHECK(r && near(si, ok[i].si, 1e-12), "'%s' as %s -> %.12g (want %.12g) %s", ok[i].text, dimension_name(ok[i].dim), si, ok[i].si, r ? "" : err);
    }
    struct {
        const char *text;
        Dimension dim;
    } bad[] = {
        {"12", DIM_LENGTH},      {"12 kg", DIM_LENGTH},       {"abc mm", DIM_LENGTH},     {"5 foo", DIM_LENGTH},
        {"1 Nm", DIM_ENERGY},    {"1 degC/s", DIM_TEMPERATURE}, {"20 K/m", DIM_TEMPERATURE}, {"1 W/(m*K", DIM_THERMAL_CONDUCTIVITY},
        {"3 mK", DIM_TEMPERATURE}, {"1 m^", DIM_AREA},         {"1 mm)", DIM_LENGTH},      {"", DIM_LENGTH},
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        double si;
        char err[200] = "";
        CHECK(!quantity_parse(bad[i].text, bad[i].dim, &si, err, sizeof err) && err[0], "should reject '%s' as %s", bad[i].text, dimension_name(bad[i].dim));
    }
    double v;
    CHECK(unit_from_si(333.15, "degC", DIM_TEMPERATURE, &v, NULL, 0) && near(v, 60, 1e-12), "K -> degC");
    CHECK(unit_from_si(2e-4, "mm", DIM_LENGTH, &v, NULL, 0) && near(v, 0.2, 1e-12), "m -> mm");
    JsonValue *num = json_number(210);
    double si;
    CHECK(quantity_from_json(num, DIM_TEMPERATURE, "degC", &si, NULL, 0) && near(si, 483.15, 1e-12), "JSON number with default unit");
    json_free(num);
}

static void test_schema(void) {
    printf("== JSON Schema validation\n");
    const char *root_text =
        "{\"$defs\": {\"q\": {\"oneOf\": ["
        "  {\"type\": \"object\", \"properties\": {\"box\": {\"type\": \"array\", \"items\": {\"type\": \"number\"}, \"minItems\": 6, \"maxItems\": 6}},"
        "   \"required\": [\"box\"], \"additionalProperties\": false},"
        "  {\"type\": \"object\", \"properties\": {\"all\": {\"type\": \"array\", \"items\": {\"$ref\": \"#/$defs/q\"}, \"minItems\": 1}},"
        "   \"required\": [\"all\"], \"additionalProperties\": false}]}},"
        " \"schema\": {\"type\": \"object\", \"properties\": {"
        "  \"units\": {\"type\": \"string\", \"enum\": [\"mm\", \"cm\", \"m\"], \"description\": \"length unit of the file\"},"
        "  \"size\": {\"type\": [\"number\", \"string\"], \"x-unit\": \"mm\", \"x-dimension\": \"length\", \"minimum\": 0},"
        "  \"n\": {\"type\": \"integer\", \"default\": 5, \"minimum\": 1},"
        "  \"opts\": {\"type\": \"object\", \"properties\": {\"flag\": {\"type\": \"boolean\", \"default\": true}}, \"additionalProperties\": false, \"default\": {}},"
        "  \"q\": {\"$ref\": \"#/$defs/q\"}},"
        " \"required\": [\"units\"], \"additionalProperties\": false}}";
    JsonError jerr;
    JsonValue *root = json_parse(root_text, strlen(root_text), NULL, &jerr);
    CHECK(root != NULL, "schema parse: %s", jerr.message);
    if (!root) return;
    const JsonValue *schema = json_get(root, "schema");
    char cerr[300];
    CHECK(jschema_check(schema, root, cerr, sizeof cerr), "schema check: %s", cerr);

    struct {
        const char *inst;
        bool valid;
        const char *expect; /* substring of the report */
    } cases[] = {
        {"{\"units\": \"mm\", \"size\": \"0.5 in\"}", true, NULL},
        {"{\"units\": \"mm\", \"size\": 12.5, \"q\": {\"all\": [{\"box\": [0,0,0,1,1,1]}]}}", true, NULL},
        {"{\"units\": \"inch\"}", false, "must be one of"},
        {"{\"units\": \"mn\"}", false, "did you mean \"mm\""},
        {"{\"unit\": \"mm\"}", false, "did you mean \"units\""},
        {"{\"size\": 1}", false, "missing required property \"units\": length unit"},
        {"{\"units\": \"mm\", \"size\": \"5 kg\"}", false, "invalid length"},
        {"{\"units\": \"mm\", \"size\": -1}", false, ">= 0"},
        {"{\"units\": \"mm\", \"n\": 2.5}", false, "integer"},
        {"{\"units\": \"mm\", \"q\": {\"all\": [{\"box\": [0,0,0,1,1]}]}}", false, "/q/all/0/box"},
        {"{\"units\": \"mm\", \"q\": {\"box\": [0,0,0,1,1,1], \"all\": []}}", false, "unknown property"},
        {"[1]", false, "must be object"},
        {"{\"units\": \"mm\", \"size\": true}", false, "a number in mm"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        JsonValue *inst = json_parse(cases[i].inst, strlen(cases[i].inst), NULL, &jerr);
        CHECK(inst != NULL, "instance %zu parse", i);
        if (!inst) continue;
        JsonSchemaReport rep;
        bool valid = jschema_validate(schema, root, inst, true, &rep);
        char text[1024];
        jschema_report_text(&rep, text, sizeof text);
        CHECK(valid == cases[i].valid, "case %zu validity (%s): %s", i, cases[i].inst, text);
        if (!cases[i].valid && cases[i].expect) CHECK(strstr(text, cases[i].expect) != NULL, "case %zu message '%s' lacks '%s'", i, text, cases[i].expect);
        if (i == 0) {
            CHECK(json_get_num(inst, "n", 0) == 5, "default applied");
            CHECK(json_get_bool(json_get(inst, "opts"), "flag", false) == true, "nested default applied inside default object");
        }
        json_free(inst);
    }
    const char *typo = "{\"type\": \"object\", \"requried\": [\"a\"]}";
    JsonValue *ts = json_parse(typo, strlen(typo), NULL, NULL);
    CHECK(ts && !jschema_check(ts, root, cerr, sizeof cerr) && strstr(cerr, "requried"), "schema typo detected: %s", cerr);
    json_free(ts);
    const char *baddef = "{\"type\": \"integer\", \"minimum\": 3, \"default\": 1}";
    ts = json_parse(baddef, strlen(baddef), NULL, NULL);
    CHECK(ts && !jschema_check(ts, root, cerr, sizeof cerr) && strstr(cerr, "default"), "invalid default detected: %s", cerr);
    json_free(ts);
    json_free(root);
}

static void test_paths(void) {
    printf("== paths\n");
    char base[NV_PATH_MAX], out[NV_PATH_MAX], err[512];
    const char *tmp = getenv("TMPDIR");
    snprintf(base, sizeof base, "%s/nv_coretest_%ld", tmp ? tmp : "/tmp", (long)getpid());
    CHECK(path_mkdirs(base), "mkdirs %s", base);
    char want[NV_PATH_MAX];
    snprintf(want, sizeof want, "%s/a/b/c.json", base);
    CHECK(path_resolve_new(want, out, sizeof out, true, false, err, sizeof err), "resolve new: %s", err);
    char real_base[NV_PATH_MAX];
    CHECK(path_real(base, real_base, sizeof real_base), "realpath base");
    CHECK(path_within(out, real_base), "within: %s in %s", out, real_base);
    snprintf(want, sizeof want, "%s/a/b", real_base);
    CHECK(path_is_dir(want), "intermediate directories created");
    snprintf(want, sizeof want, "%s/x/../../etc/passwd", base);
    CHECK(!path_resolve_new(want, out, sizeof out, false, false, err, sizeof err), "'..' below a missing directory rejected");
    snprintf(want, sizeof want, "%s/a/../a/b/d.txt", base);
    CHECK(path_resolve_new(want, out, sizeof out, false, false, err, sizeof err) && path_within(out, real_base), "'..' inside existing part resolved: %s", out);
    CHECK(path_within("/a/b", "/a") && !path_within("/ab", "/a") && path_within("/a", "/a") && !path_within("/", "/a"), "prefix semantics");
    snprintf(want, sizeof want, "%s/a/b/file.txt", real_base);
    CHECK(path_write_file_atomic(want, "hello", 5, err, sizeof err), "atomic write: %s", err);
    size_t n;
    char *data = path_read_file(want, 100, &n, err, sizeof err);
    CHECK(data && n == 5 && !memcmp(data, "hello", 5), "read back");
    free(data);
    CHECK(path_read_file(want, 3, &n, err, sizeof err) == NULL, "size limit enforced");
    char cmd[NV_PATH_MAX + 16];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", real_base);
    if (system(cmd) != 0) printf("  (cleanup failed)\n");
}

int main(void) {
    test_json_valid();
    test_json_invalid();
    test_sha_base64();
    test_units();
    test_schema();
    test_paths();
    printf("\n%s: %d passed, %d failed\n", g_fail ? "CORE TESTS FAILED" : "ALL CORE TESTS PASSED", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
