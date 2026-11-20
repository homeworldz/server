// The LSL semantics checks of the compatibility suite.
//
// Each check compiles a small LSL script, runs it on a Falcon VM with the test
// host in harness.h, and compares what the script said with what Second Life
// does. Expected values come from Second Life's documented behaviour (Linden
// Lab's definitions) or, for digests, Base64 and exact 32-bit arithmetic, from
// Python's hashlib, hmac, base64 and integers; RSA from OpenSSL. The Halcyon
// rules were measured on Halcyon's own compiler.
//
// Written by Ashley of Rookery Isles for the Rookery scripting package
// (lsl_semantics_test.cpp, 2026-11-14 to 2026-11-18) and ported here. The
// check bodies are hers. What changed, because Falcon has one dialect (Second
// Life's LSL plus Halcyon's rules, no OpenSimulator, docs/SCRIPTING.md):
//   - the OpenSimulator checks (implicit string conversion, os* overloads) and
//     every "still refused in the sl dialect" check are gone;
//   - a check that something is refused also requires the nearest valid form
//     to compile, so it cannot pass only because Falcon refuses everything
//     around it;
//   - integer literals past 32 bits are checked against Second Life's rule
//     alone (InWorldz gave -1; a per-script dialect is not decided);
//   - a function the region does not answer does not compile on Falcon, so the
//     "unbound host function is a runtime error" check is gone, and the check
//     that used one to raise a runtime error divides by zero instead.
//
// Most of these do not pass yet. That is the point: compat_test.cpp records
// which do, and fails when that set changes without the record being updated.

#include "harness.h"
#include "rsa_vectors.h"

#include <string>
#include <vector>

namespace compat {
namespace {

void integers() {
    expect("integer + wraps at 2^31", says("integer big = 2147483647; integer small = -2147483648;",
                                           {"llOwnerSay((string)(big + 1));", "llOwnerSay((string)(small - 1));"}),
           {"-2147483648", "2147483647"});
    expect("integer * wraps exactly (Python: 123456789*1000 -> -1097262584)",
           says("integer a = 123456789; integer b = 65536; integer c = 46341;",
                {"llOwnerSay((string)(a * 1000));", "llOwnerSay((string)(b * b));", "llOwnerSay((string)(c * c));"}),
           {"-1097262584", "0", "-2147479015"});
    expect("integer division truncates toward zero",
           says("integer m = 7; integer n = -7;",
                {"llOwnerSay((string)(m / 2));", "llOwnerSay((string)(n / 2));", "llOwnerSay((string)(n % 2));",
                 "llOwnerSay((string)(m % -2));"}),
           {"3", "-3", "-1", "1"});
    expect("INT_MIN / -1 is INT_MIN, INT_MIN % -1 is 0",
           says("integer lo = -2147483648; integer neg = -1;",
                {"llOwnerSay((string)(lo / neg));", "llOwnerSay((string)(lo % neg));"}),
           {"-2147483648", "0"});
    expect("shifts mask the count and keep the sign",
           says("integer one = 1; integer m8 = -8;",
                {"llOwnerSay((string)(one << 31));", "llOwnerSay((string)(one << 32));",
                 "llOwnerSay((string)(m8 >> 1));", "llOwnerSay((string)(~0));", "llOwnerSay((string)(0xF0 & 0x3C));",
                 "llOwnerSay((string)(0xF0 | 0x0F));", "llOwnerSay((string)(0xFF ^ 0x0F));"}),
           {"-2147483648", "1", "-4", "-1", "48", "255", "240"});
    expect("unary minus wraps, ++ and -- pre and post",
           says("integer lo = -2147483648; integer i = 5; integer j; integer k;",
                {"llOwnerSay((string)(-lo));", "j = i++;", "k = ++i;",
                 "llOwnerSay((string)i + \" \" + (string)j + \" \" + (string)k);", "j = i--;", "k = --i;",
                 "llOwnerSay((string)i + \" \" + (string)j + \" \" + (string)k);"}),
           {"-2147483648", "7 5 7", "5 7 5"});
    expect("compound assignments, including integer *= float",
           says("integer i = 10; float f = 1.5; string s = \"a\"; list l;",
                {"i += 5; i -= 3; i *= 2; i /= 5; i %= 3;", "llOwnerSay((string)i);", "i = 5; i *= 1.5;",
                 "llOwnerSay((string)i);", "f += 1; f *= 2;", "llOwnerSay((string)f);", "s += \"b\";",
                 "llOwnerSay(s);", "l += 1; l += [2.5, \"x\"];", "llOwnerSay((string)llGetListLength(l));"}),
           // ((10 + 5 - 3) * 2 / 5) % 3 = 1; 5 * 1.5 = 7.5 stored as 7
           {"1", "7", "5.000000", "ab", "3"});
    expect("comparisons and logic give 1 and 0, && and || without short circuit",
           says("integer a = 3; integer b = 4;",
                {"llOwnerSay((string)(a < b) + (string)(a > b) + (string)(a <= 3) + (string)(a >= 4));",
                 "llOwnerSay((string)(a == 3) + (string)(a != 3) + (string)(!a) + (string)(!0));",
                 "llOwnerSay((string)(a && 0) + (string)(a || 0) + (string)(0 || 0));"}),
           {"1010", "1001", "010"});
}

void division_by_zero() {
    const std::string src =
        "default {\n"
        " state_entry() {\n"
        "  integer zero = 0;\n"
        "  llOwnerSay(\"before\");\n"
        "  llOwnerSay((string)(5 / zero));\n"
        "  llOwnerSay(\"not reached\");\n"
        " }\n"
        " touch_start(integer n) {\n"
        "  float z = 0.0;\n"
        "  llOwnerSay((string)(1.0 / z));\n"
        " }\n"
        " timer() {\n"
        "  integer zero = 0;\n"
        "  llOwnerSay((string)(5 % zero));\n"
        " }\n"
        "}\n";
    Session s(src);
    if (!s.script) {
        check(false, "Math Error", s.compile_error);
        return;
    }
    s.drain();
    check(s.host.said.size() == 1 && s.host.errors.size() == 1 && s.host.errors[0].message == "Math Error" &&
              s.host.errors[0].lsl_line == 5,
          "integer / 0 raises Math Error at its LSL line (5) and ends the event",
          s.host.errors.empty() ? "no error" : s.host.errors[0].message + " line " + std::to_string(s.host.errors[0].lsl_line));
    s.script->post_event("touch_start", {V::integer(1)});
    s.script->post_event("timer", {});
    s.drain();
    check(s.host.errors.size() == 3 && s.host.errors[1].message == "Math Error" && s.host.errors[1].lsl_line == 10 &&
              s.host.errors[2].message == "Math Error" && s.host.errors[2].lsl_line == 14,
          "float / 0.0 and integer % 0 raise Math Error (lines 10, 14); the script keeps running",
          std::to_string(s.host.errors.size()) + " errors");
}

void floats() {
    expect("float32 at stores, function boundaries and list insertion",
           says("float half(float x) { return x; }",
                {"float f = 16777217;", "llOwnerSay((string)f);", "llOwnerSay((string)half(16777217));",
                 "list l = [16777217.0];", "llOwnerSay(llList2String(l, 0));",
                 "integer i = 16777217;", "llOwnerSay((string)i);"}),
           {"16777216.000000", "16777216.000000", "16777216.000000", "16777217"});
    expect("float casts: six decimals, Infinity, NaN",
           says("float big = 1e30; float inf; float nan;",
                {"llOwnerSay((string)1.5 + \" \" + (string)(-0.25) + \" \" + (string)0.1);",
                 "inf = (float)\"inf\"; nan = (float)\"nan\";",
                 "llOwnerSay((string)inf + \" \" + (string)(-inf) + \" \" + (string)nan);",
                 "llOwnerSay((string)big);"}),
           {"1.500000 -0.250000 0.100000", "Infinity -Infinity NaN", "1000000015047466219876688855040.000000"});
    expect("float to integer truncates; out of range is INT_MIN",
           says("float a = 1.9; float b = -1.9; float c = 1e20;",
                {"llOwnerSay((string)((integer)a) + \" \" + (string)((integer)b) + \" \" + (string)((integer)c));"}),
           {"1 -1 -2147483648"});
}

void casts() {
    expect("string to integer: decimal, hex, junk, saturation",
           says("",
                {"llOwnerSay((string)((integer)\"0x1A\") + \" \" + (string)((integer)\"  42abc\") + \" \" + "
                 "(string)((integer)\"abc\") + \" \" + (string)((integer)\"-17\") + \" \" + "
                 "(string)((integer)\"4294967296\"));"}),
           {"26 42 0 -17 -1"});
    expect("string to float, vector and rotation",
           says("",
                {"llOwnerSay((string)((float)\"1.5e2\") + \" \" + (string)((float)\"  -3.25xyz\"));",
                 "llOwnerSay((string)((vector)\"<1, 2, 3>\"));", "llOwnerSay((string)((vector)\"junk\"));",
                 "llOwnerSay((string)((rotation)\"<0.1,0.2,0.3,0.4>\"));"}),
           {"150.000000 -3.250000", "<1.00000, 2.00000, 3.00000>", "<0.00000, 0.00000, 0.00000>",
            "<0.10000, 0.20000, 0.30000, 0.40000>"});
    expect("list to string concatenates each element's cast",
           says("", {"llOwnerSay((string)[1, 2.5, \"a\", <1,2,3>, (key)\"k\"]);"}),
           {"12.500000a<1.00000, 2.00000, 3.00000>k"});
    expect("key and string convert both ways; key truthiness needs a valid non-null UUID",
           says("key k = \"a822ff2b-ff02-461d-b45d-dcd10a2de0c2\"; key nk = NULL_KEY; key junk = \"abc\";",
                {"string s = k;", "llOwnerSay(s);", "if (k) llOwnerSay(\"valid\");", "if (nk) llOwnerSay(\"null\");",
                 "if (junk) llOwnerSay(\"junk\");", "if (k == \"a822ff2b-ff02-461d-b45d-dcd10a2de0c2\") llOwnerSay(\"eq\");"}),
           {"a822ff2b-ff02-461d-b45d-dcd10a2de0c2", "valid", "eq"});
}

void list_typing() {
    expect("llGetListEntryType keeps every element's type, key included",
           // (casts are not constant, so the lists are built in the handler, not as globals)
           says("",
                {"list l = [1, 1.0, \"s\", (key)\"k\", <1,2,3>, <0,0,0,1>];", "string out; integer i;",
                 "for (i = 0; i < 7; ++i) out += (string)llGetListEntryType(l, i);", "llOwnerSay(out);",
                 "list m = [\"a\"]; m += (key)\"b\"; m = llList2List(m, 1, 1);",
                 "llOwnerSay((string)llGetListEntryType(m, 0) + (string)llGetListEntryType(l, -1));"}),
           {"1234560", "46"});
    expect("lists have value semantics",
           says("", {"list a = [1, 2]; list b = a; b += 3;",
                     "llOwnerSay((string)llGetListLength(a) + (string)llGetListLength(b));",
                     "vector v = <1,2,3>; vector w = v; w.x = 9;", "llOwnerSay((string)v.x + \" \" + (string)w.x);"}),
           {"23", "1.000000 9.000000"});
    expect("list == compares lengths, != gives the length difference",
           says("", {"llOwnerSay((string)([1, 2] == [3, 4]) + \" \" + (string)([1, 2, 3] != [1]));"}), {"1 2"});
    expect("llList2* conversions",
           says("",
                {"list l = [7, 2.75, \"12abc\", <1,2,3>, (key)\"k\", \"<4,5,6>\"];",
                 "llOwnerSay((string)llList2Integer(l, 0) + \" \" + (string)llList2Integer(l, 1) + \" \" + "
                 "(string)llList2Integer(l, 2) + \" \" + (string)llList2Integer(l, 3) + \" \" + "
                 "(string)llList2Integer(l, 99));",
                 "llOwnerSay((string)llList2Float(l, 0) + \" \" + llList2String(l, 1) + \" \" + llList2String(l, 3));",
                 "llOwnerSay((string)llList2Vector(l, 5) + \" \" + (string)llList2Vector(l, 0));",
                 "llOwnerSay((string)llList2Key(l, 4) + \"|\" + (string)llList2Key(l, 0) + \"|\");"}),
           {"7 2 12 0 0", "7.000000 2.750000 <1.00000, 2.00000, 3.00000>",
            "<4.00000, 5.00000, 6.00000> <0.00000, 0.00000, 0.00000>", "k||"});
}

void list_ops() {
    expect("llList2List and llDeleteSubList, with negative and inverted ranges",
           says("list l = [0, 1, 2, 3, 4, 5];",
                {"llOwnerSay((string)llList2List(l, 1, 3) + \" \" + (string)llList2List(l, 4, 1) + \" \" + "
                 "(string)llList2List(l, -2, -1));",
                 "llOwnerSay((string)llDeleteSubList(l, 1, 3) + \" \" + (string)llDeleteSubList(l, 3, 1) + \" \" + "
                 "(string)llDeleteSubList(l, 0, -1) + \"|\");"}),
           {"123 0145 45", "045 2 |"});
    expect("llListInsertList, llListReplaceList, llList2ListStrided",
           says("list l = [0, 1, 2, 3];",
                {"llOwnerSay((string)llListInsertList(l, [\"x\"], 2) + \" \" + (string)llListInsertList(l, [\"x\"], -1) + "
                 "\" \" + (string)llListInsertList(l, [\"x\"], 9));",
                 "llOwnerSay((string)llListReplaceList(l, [\"x\", \"y\"], 1, 2) + \" \" + "
                 "(string)llListReplaceList(l, [\"x\"], 3, 1));",
                 "llOwnerSay((string)llList2ListStrided([0, 1, 2, 3, 4, 5, 6, 7], 0, -1, 2) + \" \" + "
                 "(string)llList2ListStrided([0, 1, 2, 3, 4, 5, 6, 7], 1, 6, 3));"}),
           {"01x23 012x3 0123x", "0xy3 2x", "0246 36"});
    expect("llListFindList: types must match, an empty test is found at 0",
           says("list l = [1, 2, 3, 2, \"2\"];",
                {"llOwnerSay((string)llListFindList(l, [2]) + \" \" + (string)llListFindList(l, [3, 2]) + \" \" + "
                 "(string)llListFindList(l, [\"2\"]) + \" \" + (string)llListFindList(l, [9]) + \" \" + "
                 "(string)llListFindList(l, []) + \" \" + (string)llListFindList([\"1\"], [1]));"}),
           {"1 2 4 -1 0 -1"});
    expect("llListSort ascending, descending and strided",
           says("",
                {"llOwnerSay((string)llListSort([3, 1, 2], 1, TRUE) + \" \" + (string)llListSort([3, 1, 2], 1, FALSE));",
                 "llOwnerSay(llDumpList2String(llListSort([\"b\", 2, \"a\", 1], 2, TRUE), \",\"));",
                 "llOwnerSay(llDumpList2String(llListSort([\"b\", \"a\", \"C\"], 1, TRUE), \",\"));"}),
           {"123 321", "a,1,b,2", "C,a,b"});
    expect("llDumpList2String, llList2CSV and llCSV2List",
           says("",
                {"llOwnerSay(llDumpList2String([1, 2.5, \"x\"], \"|\"));",
                 "llOwnerSay(llList2CSV([1, <1,2,3>, \"a b\"]));",
                 "list c = llCSV2List(\"a, <1,2,3>, b \");",
                 "llOwnerSay((string)llGetListLength(c) + \"|\" + llList2String(c, 1) + \"|\" + llList2String(c, 2) + \"|\");"}),
           {"1|2.500000|x", "1, <1.000000, 2.000000, 3.000000>, a b", "3|<1,2,3>|b|"});
    expect("llListStatistics",
           says("list n = [1, 2.0, \"skip\", 3, 4];",
                {"llOwnerSay((string)llListStatistics(LIST_STAT_MEAN, n) + \" \" + "
                 "(string)llListStatistics(LIST_STAT_MEDIAN, n) + \" \" + (string)llListStatistics(LIST_STAT_SUM, n) + "
                 "\" \" + (string)llListStatistics(LIST_STAT_NUM_COUNT, n) + \" \" + "
                 "(string)llListStatistics(LIST_STAT_RANGE, n));"}),
           {"2.500000 2.500000 10.000000 4.000000 3.000000"});
}

void strings() {
    expect("llStringLength and llGetSubString count code points",
           says("string s = \"h\xC3\xA9llo\";",
                {"llOwnerSay((string)llStringLength(s) + \" \" + llGetSubString(s, 1, 1) + \" \" + llGetSubString(s, -3, -1));",
                 "llOwnerSay(llGetSubString(\"Hello\", 1, 3) + \" \" + llGetSubString(\"Hello\", 3, 1) + \" \" + "
                 "llGetSubString(\"Hello\", 10, 20) + \"|\");"}),
           {"5 \xC3\xA9 llo", "ell Helo |"});
    expect("llDeleteSubString, llInsertString, llSubStringIndex",
           says("",
                {"llOwnerSay(llDeleteSubString(\"Hello\", 1, 3) + \" \" + llDeleteSubString(\"Hello\", 3, 1) + \" \" + "
                 "llDeleteSubString(\"abcdef\", 3, 0));",
                 "llOwnerSay(llInsertString(\"Hello\", 2, \"XX\") + \" \" + llInsertString(\"Hi\", 9, \"!\"));",
                 "llOwnerSay((string)llSubStringIndex(\"Hello\", \"ll\") + \" \" + (string)llSubStringIndex(\"Hello\", \"z\") + "
                 "\" \" + (string)llSubStringIndex(\"h\xC3\xA9llo\", \"llo\"));"}),
           {"Ho l bc", "HeXXllo Hi!", "2 -1 2"});
    expect("llToUpper, llToLower (Unicode), llStringTrim",
           says("",
                {"llOwnerSay(llToUpper(\"h\xC3\xA9llo \xCF\x89\") + \" \" + llToLower(\"\xC3\x89T\xC3\x89\"));",
                 "llOwnerSay(\"[\" + llStringTrim(\"  a b \\n\", STRING_TRIM) + \"][\" + "
                 "llStringTrim(\"  a \", STRING_TRIM_HEAD) + \"][\" + llStringTrim(\" a  \", STRING_TRIM_TAIL) + \"]\");"}),
           {"H\xC3\x89LLO \xCE\xA9 \xC3\xA9t\xC3\xA9", "[a b][a ][ a]"});
    expect("llParseString2List, llParseStringKeepNulls, spacers",
           says("",
                {"llOwnerSay(llDumpList2String(llParseString2List(\"a,b,,c\", [\",\"], []), \"|\"));",
                 "llOwnerSay(llDumpList2String(llParseStringKeepNulls(\"a,b,,c\", [\",\"], []), \"|\"));",
                 "llOwnerSay(llDumpList2String(llParseString2List(\"1+2-3\", [], [\"+\", \"-\"]), \"|\"));",
                 "llOwnerSay((string)llGetListLength(llParseStringKeepNulls(\"\", [\",\"], [])));"}),
           {"a|b|c", "a|b||c", "1|+|2|-|3", "1"});
    expect("llReplaceSubString, llChar, llOrd",
           says("",
                {"llOwnerSay(llReplaceSubString(\"aaaa\", \"a\", \"b\", 2) + \" \" + llReplaceSubString(\"aaaa\", \"a\", \"b\", -2) "
                 "+ \" \" + llReplaceSubString(\"aaaa\", \"a\", \"b\", 0));",
                 "llOwnerSay(llChar(233) + \" \" + (string)llOrd(\"\xC3\xA9\", 0) + \" \" + (string)llOrd(\"abc\", -1));"}),
           {"bbaa aabb bbbb", "\xC3\xA9 233 99"});
}

void encodings() {
    expect("Base64 (Python base64)",
           says("",
                {"llOwnerSay(llStringToBase64(\"Hello\") + \" \" + llBase64ToString(\"SGVsbG8=\"));",
                 "llOwnerSay(llIntegerToBase64(1) + \" \" + llIntegerToBase64(-1) + \" \" + llIntegerToBase64(-559038737));",
                 "llOwnerSay((string)llBase64ToInteger(\"AAAAAQ==\") + \" \" + (string)llBase64ToInteger(\"/////w==\") + \" \" + "
                 "(string)llBase64ToInteger(\"3q2+7w==\"));",
                 "llOwnerSay(llXorBase64(\"SGVsbG8gV29ybGQ=\", \"a2V5\"));"}),
           {"SGVsbG8= Hello", "AAAAAQ== /////w== 3q2+7w==", "1 -1 -559038737", "IwAVBwpZPAoLBwE="});
    expect("digests match Python hashlib (llMD5String hashes src + \":\" + nonce)",
           says("",
                {"llOwnerSay(llMD5String(\"Hello, Avatar!\", 0));", "llOwnerSay(llSHA1String(\"Hello\"));",
                 "llOwnerSay(llSHA256String(\"Hello\"));", "llOwnerSay(llComputeHash(\"abc\", \"sha224\"));",
                 "llOwnerSay(llComputeHash(\"abc\", \"sha384\"));", "llOwnerSay(llComputeHash(\"abc\", \"sha512\"));"}),
           {"112abd47ceaae1c05a826828650434a6", "f7ff9e8b7bb2e09b70935a5d785e0cc5d9d0abf0",
            "185f8db32271fe25f561a6fc938b2e264306ec304eda518007d1764826381969",
            "23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7",
            "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7",
            "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"});
    expect("llHMAC (Python hmac), including a key longer than the block",
           says("",
                {"llOwnerSay(llHMAC(\"key\", \"The quick brown fox jumps over the lazy dog\", \"sha256\"));",
                 "llOwnerSay(llHMAC(\"key\", \"The quick brown fox jumps over the lazy dog\", \"md5\"));",
                 "string k = \"k\"; integer i; for (i = 1; i < 200; ++i) k += \"k\";",
                 "llOwnerSay(llHMAC(k, \"msg\", \"sha512\"));"}),
           {"97yD9DBThCSxMpjmqm+xQ+9NWaFJRhdZl0edvC0aPNg=", "gAcHE0Y+d0m5DC3CSRHidQ==",
            "tSRZcb61KlqYaBLEZmoFxzW/W7eroy6uIZKtrWBd9BEtbChdHEbPgcy3q4wsO3s8Z5MhaQm1rdBSI+0h8kzbHg=="});
    expect("JSON functions",
           says("",
                {"llOwnerSay(llList2Json(JSON_ARRAY, [1, 2.5, \"a\", JSON_TRUE]));",
                 "llOwnerSay(llList2Json(JSON_OBJECT, [\"k\", 1, \"n\", \"[1,2]\"]));",
                 "llOwnerSay(llJsonGetValue(\"{\\\"a\\\":[1,2,{\\\"b\\\":\\\"c\\\"}]}\", [\"a\", 2, \"b\"]));",
                 "llOwnerSay((string)(llJsonValueType(\"{\\\"a\\\":[1,2]}\", [\"a\"]) == JSON_ARRAY) + "
                 "(string)(llJsonGetValue(\"[1]\", [5]) == JSON_INVALID));",
                 "llOwnerSay(llJsonSetValue(\"[1,2]\", [JSON_APPEND], \"3\") + \" \" + "
                 "llJsonSetValue(\"{\\\"a\\\":1}\", [\"b\", 0], \"x\") + \" \" + llJsonSetValue(\"[1,2,3]\", [1], JSON_DELETE));",
                 "list j = llJson2List(\"[1,\\\"x\\\",true,2.5]\");",
                 "llOwnerSay((string)llGetListEntryType(j, 0) + (string)llGetListEntryType(j, 1) + "
                 "(string)(llList2String(j, 2) == JSON_TRUE) + (string)llGetListEntryType(j, 3));"}),
           {"[1,2.500000,\"a\",true]", "{\"k\":1,\"n\":[1,2]}", "c", "11", "[1,2,3] {\"a\":1,\"b\":[\"x\"]} [1,3]",
            "1312"});
}

void rsa_functions() {
    const std::string globals = std::string("string key1 = \"") + kKey1 + "\";\nstring pub1 = \"" + kPub1 +
                                "\";\nstring key2 = \"" + kKey2 + "\";\nstring pub2 = \"" + kPub2 + "\";\n";
    expect("llSignRSA gives OpenSSL's signatures (PKCS#1 and PKCS#8 private keys)",
           says(globals, {"llOwnerSay(llSignRSA(key1, \"Hello, Avatar!\", \"sha256\"));",
                          "llOwnerSay(llSignRSA(key2, \"Hello, Avatar!\", \"sha1\"));",
                          "llOwnerSay(llSignRSA(key2, \"Hello, Avatar!\", \"sha512\"));",
                          "llOwnerSay(\"[\" + llSignRSA(\"not a key\", \"x\", \"sha256\") + \"]\");"}),
           {kSig1Sha256, kSig2Sha1, kSig2Sha512, "[]"});
    expect("llVerifyRSA accepts OpenSSL's signatures and refuses a changed message or signature",
           says(globals, {std::string("llOwnerSay((string)llVerifyRSA(pub1, \"Hello, Avatar!\", \"") + kSig1Sha256 +
                              "\", \"sha256\") + (string)llVerifyRSA(pub2, \"Hello, Avatar!\", \"" + kSig2Sha512 +
                              "\", \"sha512\"));",
                          std::string("llOwnerSay((string)llVerifyRSA(pub1, \"Hello, Avatar?\", \"") + kSig1Sha256 +
                              "\", \"sha256\") + (string)llVerifyRSA(pub2, \"Hello, Avatar!\", \"" + kSig2Sha512 +
                              "\", \"sha256\") + (string)llVerifyRSA(pub1, \"Hello, Avatar!\", \"AAAA\", \"sha256\"));"}),
           {"11", "000"});
}

void math_functions() {
    expect("integer and rounding functions (Linden Lab's definitions)",
           says("",
                {"llOwnerSay((string)llAbs(-5) + \" \" + (string)llAbs(-2147483648) + \" \" + (string)llFloor(-1.5) + \" \" + "
                 "(string)llCeil(-1.5) + \" \" + (string)llRound(2.5) + \" \" + (string)llRound(-2.5));",
                 "llOwnerSay((string)llModPow(2, 10, 1000) + \" \" + (string)llModPow(3, 200, 1000000007) + \" \" + "
                 "(string)llModPow(-1, 3, -5));"}),
           {"5 -2147483648 -2 -1 3 -2", "24 136318165 64"});
    expect("float functions: llPow, llSqrt (negative is NaN), llLog (negative is 0.0)",
           says("",
                {"llOwnerSay((string)llPow(2, 10) + \" \" + (string)llSqrt(16) + \" \" + (string)llSqrt(-1) + \" \" + "
                 "(string)llLog(-1) + \" \" + (string)llLog10(1000) + \" \" + (string)llFabs(-2.5));"}),
           {"1024.000000 4.000000 NaN 0.000000 3.000000 2.500000"});
}

void vectors_rotations() {
    expect("vector operators: dot, cross, scale, member assignment",
           says("vector v = <1,2,3>;",
                {"llOwnerSay((string)(v * <4,5,6>) + \" \" + (string)(<1,0,0> % <0,1,0>) + \" \" + (string)(v * 2) + \" \" + "
                 "(string)(v / 2));",
                 "v.y = 5; v.z += 1;", "llOwnerSay((string)v + \" \" + (string)(-v) + \" \" + (string)(v - <1,1,1>));",
                 "llOwnerSay((string)llVecMag(<3,4,0>) + \" \" + (string)llVecNorm(<0,0,5>) + \" \" + "
                 "(string)llVecDist(<1,1,1>, <1,1,3>) + \" \" + (string)llVecNorm(ZERO_VECTOR));"}),
           {"32.000000 <0.00000, 0.00000, 1.00000> <2.00000, 4.00000, 6.00000> <0.50000, 1.00000, 1.50000>",
            "<1.00000, 5.00000, 4.00000> <-1.00000, -5.00000, -4.00000> <0.00000, 4.00000, 3.00000>",
            "5.000000 <0.00000, 0.00000, 1.00000> 2.000000 <0.00000, 0.00000, 0.00000>"});
    const std::string near =
        "integer near(vector a, vector b) { return llVecDist(a, b) < 0.00001; }\n"
        "integer rnear(rotation a, rotation b) { return llAngleBetween(a, b) < 0.0001; }\n";
    expect("rotations: v * r turns +X to +Y for 90 degrees about Z; composition, division",
           says(near,
                {"rotation z90 = llEuler2Rot(<0, 0, PI_BY_TWO>);", "rotation x90 = llEuler2Rot(<PI_BY_TWO, 0, 0>);",
                 "vector v = <1, 2, 3>;",
                 "llOwnerSay((string)near(<1,0,0> * z90, <0,1,0>) + (string)near((v * z90) * x90, v * (z90 * x90)) + "
                 "(string)rnear((z90 * x90) / x90, z90) + (string)near((v * z90) / z90, v));",
                 "rotation r = <1, 2, 3, 4>; r.s = 9;", "llOwnerSay((string)r + \" \" + (string)(r == <1,2,3,9>));"}),
           {"1111", "<1.00000, 2.00000, 3.00000, 9.00000> 1"});
    expect("rotation library functions",
           says(near,
                {"llOwnerSay((string)near(llRot2Euler(llEuler2Rot(<0.1, 0.2, 0.3>)), <0.1, 0.2, 0.3>) + "
                 "(string)rnear(llEuler2Rot(<PI, 0, 0>), <1, 0, 0, 0>));",
                 "rotation aa = llAxisAngle2Rot(<0, 0, 2>, PI_BY_TWO);",
                 "llOwnerSay((string)near(llRot2Axis(aa), <0,0,1>) + (string)(llFabs(llRot2Angle(aa) - PI_BY_TWO) < 0.0001));",
                 "llOwnerSay((string)near(<1,0,0> * llRotBetween(<1,0,0>, <0,2,0>), <0,1,0>) + "
                 "(string)near(<1,0,0> * llRotBetween(<1,0,0>, <-1,0,0>), <-1,0,0>));",
                 "rotation q = llEuler2Rot(<0.3, -0.2, 1.1>);",
                 "llOwnerSay((string)near(llRot2Fwd(q), <1,0,0> * q) + (string)near(llRot2Left(q), <0,1,0> * q) + "
                 "(string)near(llRot2Up(q), <0,0,1> * q) + (string)rnear(llAxes2Rot(llRot2Fwd(q), llRot2Left(q), llRot2Up(q)), q));",
                 "llOwnerSay((string)(llFabs(llAngleBetween(ZERO_ROTATION, llEuler2Rot(<0, 0, 1.0>)) - 1.0) < 0.0001));"}),
           {"11", "11", "11", "1111", "1"});
}

void states() {
    const std::string src =
        "default {\n"
        " state_entry() { llOwnerSay(\"default entry\"); state two; }\n"
        " state_exit() { llOwnerSay(\"default exit\"); }\n"
        "}\n"
        "state two {\n"
        " state_entry() { llOwnerSay(\"two entry\"); }\n"
        " touch_start(integer n) { llOwnerSay(\"touched \" + (string)n); state three; llOwnerSay(\"not reached\"); }\n"
        " state_exit() { llOwnerSay(\"two exit\"); }\n"
        "}\n"
        "state three {\n"
        " state_entry() { llOwnerSay(\"three entry\"); }\n"
        " touch_start(integer n) { llOwnerSay(\"three touched\"); }\n"
        "}\n";
    Session s(src);
    if (!s.script) {
        check(false, "states", s.compile_error);
        return;
    }
    s.drain();
    check(joined(s.host.said) == "default entry / default exit / two entry" && s.script->current_state() == "two",
          "state change runs state_exit of the old state, then state_entry of the new one", joined(s.host.said));
    s.host.said.clear();
    s.script->post_event("touch_start", {V::integer(1)});
    s.script->post_event("touch_start", {V::integer(2)});
    s.drain();
    check(joined(s.host.said) == "touched 1 / two exit / three entry",
          "`state x;` ends the handler; events queued before the change are dropped", joined(s.host.said));
    check(joined(s.host.transitions) == "default>two / two>three", "the host sees each state change",
          joined(s.host.transitions));

    expect("a state change inside a user function ends the event",
           "go() { if (TRUE) state other; llOwnerSay(\"after state in function\"); }\n"
           "default { state_entry() { go(); llOwnerSay(\"after go\"); } }\n"
           "state other { state_entry() { llOwnerSay(\"other entry\"); } }\n",
           {"other entry"});
    expect("changing to the current state acts as return",
           "default { state_entry() { llOwnerSay(\"once\"); state default; llOwnerSay(\"no\"); } "
           "state_exit() { llOwnerSay(\"no exit\"); } }\n",
           {"once"});
}

void jumps() {
    expect("jump backward makes a loop", says("", {"integer i = 0;", "@top;", "i++;", "if (i < 5) jump top;",
                                                   "llOwnerSay((string)i);"}),
           {"5"});
    expect("jump forward skips code", says("", {"jump skip;", "llOwnerSay(\"no\");", "@skip;", "llOwnerSay(\"yes\");"}),
           {"yes"});
    expect("break-like and continue-like jumps",
           says("", {"integer i = 0;", "while (TRUE) { if (++i > 3) jump done; }", "@done;", "llOwnerSay((string)i);",
                     "string s;", "for (i = 0; i < 5; i++) { if (i % 2) jump next; s += (string)i; @next; }",
                     "llOwnerSay(s);", "i = 0; s = \"\";", "do { i++; if (i == 2) jump cont; s += (string)i; @cont; } while (i < 4);",
                     "llOwnerSay(s);"}),
           {"4", "024", "134"});
    expect("jumps between nested loops and out of a function",
           "integer find(list l, integer x) {\n"
           "  integer i; integer j;\n"
           "  for (i = 0; i < 3; i++) {\n"
           "    for (j = 0; j < 3; j++) {\n"
           "      if (llList2Integer(l, i * 3 + j) == x) jump found;\n"
           "    }\n"
           "  }\n"
           "  return -1;\n"
           "  @found;\n"
           "  return i * 10 + j;\n"
           "}\n"
           "default { state_entry() {\n"
           "  list m = [1,2,3,4,5,6,7,8,9];\n"
           "  llOwnerSay((string)find(m, 6) + \" \" + (string)find(m, 42));\n"
           "  integer k = 0; string s;\n"
           "  @again;\n"
           "  s += (string)k;\n"
           "  if (++k < 3) { jump again; }\n"
           "  llOwnerSay(s);\n"
           "} }\n",
           {"12 -1", "012"});
}

void big_functions() {
    // 160 locals: more than Luau's registers allow, so they live in a table
    std::string body;
    std::string sum = "0";
    for (int i = 0; i < 160; ++i) {
        body += "  integer v" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
        sum += " + v" + std::to_string(i);
    }
    expect("a function with 160 locals (spilled to a table) still runs",
           "integer total() {\n" + body + "  return " + sum + ";\n}\n"
           "default { state_entry() { llOwnerSay((string)total()); } }\n",
           {"12720"});
    expect("a backward jump over a declaration runs the declaration again",
           says("", {"integer n = 0;", "@again;", "integer k = 5;", "k++;", "n++;", "if (n < 3) jump again;",
                     "llOwnerSay((string)k + \" \" + (string)n);"}),
           {"6 3"});
}

void evaluation_order() {
    const std::string globals =
        "string log;\n"
        "integer f(integer x) { log += (string)x; return x; }\n"
        "integer g(integer a, integer b) { return a * 10 + b; }\n";
    expect("binary operands right to left, arguments and list elements left to right (Mono)",
           says(globals,
                {"integer r = f(1) - f(2);", "llOwnerSay(log + \" \" + (string)r);", "log = \"\";",
                 "r = g(f(1), f(2));", "llOwnerSay(log + \" \" + (string)r);", "log = \"\";",
                 "list l = [f(1), f(2), f(3)];", "llOwnerSay(log);", "log = \"\";",
                 "if (f(0) && f(7)) llOwnerSay(\"no\");", "llOwnerSay(log);", "log = \"\";",
                 "integer x = 1;", "llOwnerSay((string)(x + (x = 5)) + \" \" + (string)((x = 2) + x));"}),
           // the last line: the outer + runs its right side first, (x = 2) + x reads x (1) then
           // sets it to 2, giving 3; then x + (x = 5) sets x to 5 first, giving 10
           {"21 -1", "12 12", "123", "70", "10 3"});
}

void functions_globals() {
    expect("recursion, globals with initializers",
           "integer g = 5; float gf = 2; list gl = [g, \"x\"]; vector gv = <1,2,3>; string gs = \"hi\";\n"
           "integer fact(integer n) { if (n <= 1) return 1; return n * fact(n - 1); }\n"
           "integer fib(integer n) { if (n < 2) return n; return fib(n - 1) + fib(n - 2); }\n"
           "default { state_entry() {\n"
           "  llOwnerSay((string)fact(10) + \" \" + (string)fib(15));\n"
           "  llOwnerSay((string)g + \" \" + (string)gf + \" \" + (string)gl + \" \" + (string)gv + \" \" + gs);\n"
           "} }\n",
           {"3628800 610", "5 2.000000 5x <1.00000, 2.00000, 3.00000> hi"});
}

void host_and_clocks() {
    expect("clock functions read the host's clock (1700000000 = 2023-11-14 22:13:20 UTC, PST)",
           says("",
                {"llOwnerSay((string)llGetUnixTime() + \" \" + llGetDate() + \" \" + llGetTimestamp());",
                 "llOwnerSay((string)llGetGMTclock() + \" \" + (string)llGetWallclock());"}),
           {"1700000000 2023-11-14 2023-11-14T22:13:20.000000Z", "80000.000000 51200.000000"});

    Session s("default { state_entry() { llOwnerSay((string)llGetKey()); llOwnerSay(llGetObjectName()); "
              "llSleep(2.0); llOwnerSay(\"woke \" + (string)llGetTime()); "
              "key q = llRequestAgentData(llGetKey(), DATA_NAME); llOwnerSay(\"got \" + (string)q); } }");
    if (!s.script) {
        check(false, "host calls", s.compile_error);
        return;
    }
    RunResult r = s.drain();
    check(r.status == RunStatus::Sleeping && s.host.said.size() == 1 && s.host.said[0] == "a822ff2b-ff02-461d-b45d-dcd10a2de0c2",
          "a host function returns a key; a forced delay puts the script to sleep", joined(s.host.said));
    s.host.clock += 0.5;
    r = s.drain();
    check(r.status == RunStatus::Sleeping && s.host.said.size() == 2 && s.host.said[1] == "Rookery Box",
          "after the delay the call returns its value; llSleep sleeps again", joined(s.host.said));
    s.host.clock += 2.0;
    r = s.drain();
    check(r.status == RunStatus::Waiting && s.host.said.size() == 3 && s.host.said[2] == "woke 2.500000",
          "llGetTime counts script time; a suspended host call waits", joined(s.host.said));
    s.script->resume(V::key("0f0e0d0c-0b0a-0908-0706-050403020100"));
    r = s.drain();
    check(r.status == RunStatus::Idle && s.host.said.size() == 4 && s.host.said[3] == "got 0f0e0d0c-0b0a-0908-0706-050403020100",
          "resume() hands the result to the waiting script", joined(s.host.said));

    Session e("default { listen(integer ch, string name, key id, string msg) { llOwnerSay((string)ch + name + (string)id + msg); } "
              "link_message(integer s, integer n, string str, key id) { llOwnerSay(str + (string)n); } }");
    if (e.script) {
        e.drain();
        e.script->post_event("listen", {V::integer(7), V::string("Bob"), V::key("k1"), V::string("hi")});
        e.script->post_event("link_message", {V::integer(1), V::integer(42), V::string("msg"), V::key("")});
        std::string why;
        const bool refused = !e.script->post_event("listen", {V::integer(1)}, &why);
        e.drain();
        check(joined(e.host.said) == "7Bobk1hi / msg42" && refused, "events carry typed arguments; wrong arguments are refused",
              joined(e.host.said) + " / " + why);
    } else {
        check(false, "events", e.compile_error);
    }

    Session reset("integer count = 10;\n"
                  "default { state_entry() { llOwnerSay(\"entry \" + (string)count); }\n"
                  " touch_start(integer n) { count++; if (count > 11) llResetScript(); llOwnerSay((string)count); } }");
    if (reset.script) {
        reset.drain();
        reset.script->post_event("touch_start", {V::integer(1)});
        reset.drain();
        reset.script->post_event("touch_start", {V::integer(1)});
        reset.drain();
        check(joined(reset.host.said) == "entry 10 / 11 / entry 10", "llResetScript restores globals and re-enters default",
              joined(reset.host.said));
    } else {
        check(false, "llResetScript", reset.compile_error);
    }
}

void limits() {
    Limits small;
    small.slice_interrupts = 1000;
    Session spin("default { state_entry() { integer i; while (TRUE) { i++; } } touch_start(integer n) { llOwnerSay(\"t\"); } }",
                 small);
    if (!spin.script) {
        check(false, "budget", spin.compile_error);
        return;
    }
    int busy = 0;
    for (int i = 0; i < 50; ++i) {
        if (spin.script->run().status == RunStatus::Busy) {
            ++busy;
        }
    }
    check(busy == 50, "a runaway loop is time-sliced: run() returns Busy every slice and keeps control",
          std::to_string(busy) + " of 50");

    Limits tight;
    tight.memory_bytes = 64 * 1024;
    Session hog("default { state_entry() { list l; while (TRUE) { l += [\"0123456789abcdef\"]; } } }", tight);
    if (!hog.script) {
        check(false, "memory", hog.compile_error);
        return;
    }
    RunResult r = hog.drain(1000000);
    check(r.status == RunStatus::Stopped && hog.script->stopped() && !hog.host.errors.empty() &&
              hog.host.errors.back().message == "Stack-Heap Collision" && hog.host.errors.back().fatal,
          "a script past its memory limit stops with Stack-Heap Collision",
          hog.host.errors.empty() ? "no error" : hog.host.errors.back().message);
}

void dialects() {
    const std::string globals_src =
        "string log;\n"
        "integer f(integer x) { log += (string)x; return x; }\n"
        "integer a = f(1);\n"
        "integer b = a + f(2);\n"
        "key me = llGetKey();\n"
        "default {\n"
        " state_entry() { llOwnerSay(log + \" \" + (string)a + \" \" + (string)b + \" \" + (string)me); }\n"
        " touch_start(integer n) { llOwnerSay(log); }\n"
        "}\n";
    Session g(globals_src);
    if (g.script) {
        g.drain();
        g.script->post_event("touch_start", {V::integer(1)});
        g.drain();
        check(joined(g.host.said) == "12 1 3 a822ff2b-ff02-461d-b45d-dcd10a2de0c2 / 12",
              "InWorldz: non-constant globals run once, in order, before state_entry, with their side effects",
              joined(g.host.said));
    } else {
        check(false, "non-constant globals", g.compile_error);
    }
    const std::string params_src =
        "default { listen(integer ch, string name, string id, string msg) { llOwnerSay(id + \"|\" + msg); } "
        "dataserver(string query, string data) { llOwnerSay(query + data); } }\n";
    Session p(params_src);
    if (p.script) {
        p.drain();
        p.script->post_event("listen", {V::integer(0), V::string("Bob"), V::key("k-1"), V::string("hi")});
        p.script->post_event("dataserver", {V::key("q"), V::string("d")});
        p.drain();
        check(joined(p.host.said) == "k-1|hi / qd",
              "InWorldz: a handler may declare string where the event passes a key", joined(p.host.said));
    } else {
        check(false, "key/string handler parameters", p.compile_error);
    }
    check(compiles(params_src) &&
              !compiles("default { listen(integer ch, string name, string id, float msg) { } }"),
          "key/string handler parameters: any other mismatch is still refused");

    const std::string concat_src = "default { state_entry() { llOwnerSay(\"x\" + 1.5); } }";
    check(compiles("default { state_entry() { llOwnerSay(\"x\" + (string)1.5); } }") && !compiles(concat_src),
          "implicit string conversion is refused, as in Second Life and InWorldz");
}

void compile_errors() {
    CompileResult r = compile("default {\n state_entry() {\n  integer x = \"text\";\n }\n}\n");
    bool line3 = false;
    for (const auto& d : r.diagnostics) {
        if (d.is_error && d.line == 3) line3 = true;
    }
    check(!r.ok && line3, "a type error is reported with its LSL line");
    CompileResult iw = compile("default { state_entry() { llOwnerSay(iwReverseString(\"abc\") + (string)iwChar2Int(\"A\", 0)); } }");
    check(iw.ok, "InWorldz iw* functions are known to the compiler");
}

void inworldz() {
    expect("InWorldz string and list extensions",
           says("",
                {"llOwnerSay(iwReverseString(\"abc\") + \" \" + (string)iwChar2Int(\"A\", 0) + \" \" + iwInt2Char(66) + \" \" + "
                 "iwReplaceString(\"a-b-c\", \"-\", \"+\") + \" \" + (string)iwSubStringIndex(\"Hello\", \"LL\", 0, FALSE));",
                 "llOwnerSay((string)iwReverseList([1, 2, 3, 4], 2) + \" \" + (string)iwListRemoveDuplicates([1, 1, \"1\", 2]) + \" \" + "
                 "(string)iwListIncludesElements([1, 2, 3], [2, 3], FALSE) + (string)iwClampInt(15, 0, 10));",
                 "llOwnerSay(iwFormatString(\"{0} has {1}\", [\"Bob\", 3]) + \" \" + (string)iwVerifyType(\"12\", 0) + "
                 "(string)iwVerifyType(\"1.5\", 0) + (string)iwVerifyType(\"<1,2,3>\", 0) + (string)iwVerifyType(\"x\", 0));",
                 "llOwnerSay((string)iwMatchString(\"hello world\", \"h.*d\", 2) + (string)iwMatchString(\"aXbXc\", \"X\", 3));"}),
           {"cba 65 B a+b+c 2", "3412 112 110", "Bob has 3 1253", "12"});
}

// What a Session said, or why it could not run.
std::string run_and_say(const std::string& source) {
    Session s(source);
    if (!s.script) {
        return "does not compile: " + s.compile_error;
    }
    s.drain();
    std::string out = joined(s.host.said);
    if (!s.host.errors.empty()) {
        out += " | runtime error: " + s.host.errors[0].message + " at LSL line " +
               std::to_string(s.host.errors[0].lsl_line);
    }
    return out;
}

// InWorldz Halcyon's own rules (compiler/halcyon_source.hh). The expected
// values were measured on Halcyon's compiler, InWorldz.Phlox.dll: what it
// accepts (tools/halcyon_oracle/PhloxCheck) and the bytecode it emits.
void halcyon_rules() {
    // rule 1: globals and functions between and after the states
    const std::string after_src =
        "default { state_entry() { llOwnerSay(Greet(\"a\") + (string)g); state two; } }\n"
        "string Greet(string s) { return \"hi \" + s; }\n"
        "integer g = 7;\n"
        "state two { state_entry() { llOwnerSay(Later()); } }\n"
        "string Later() { return \"later\"; }\n";
    const std::string after = run_and_say(after_src);
    check(after == "hi a7 / later", "Halcyon: functions and globals after the states are visible everywhere", after);

    // the messages and runtime errors of a moved function keep the script's own lines
    {
        CompileResult r = compile("default { state_entry() { F(); } }\n"
                                  "F() {\n"
                                  "  integer x = \"s\";\n"
                                  "}\n");
        int line = 0;
        for (const auto& d : r.diagnostics) {
            if (d.is_error) line = d.line;
        }
        check(!r.ok && line == 3, "Halcyon: an error in a moved function is reported on its own line",
              "line " + std::to_string(line));
        Session s("default { state_entry() { F(); } }\n"
                  "\n"
                  "F() {\n"
                  "  integer zero = 0; integer q = 1 / zero;\n"
                  "}\n");
        if (s.script) {
            s.drain();
            const int at = s.host.errors.empty() ? 0 : s.host.errors[0].lsl_line;
            check(at == 4, "Halcyon: a runtime error in a moved function names its own line",
                  "LSL line " + std::to_string(at));
        } else {
            check(false, "Halcyon: a runtime error in a moved function names its own line", s.compile_error);
        }
    }

    // rule 2: a global's initializer reads a later global as its default value
    // (Phlox: gload 1 before iconst 2 / gstore 1)
    const std::string forward = run_and_say(
        "integer h = g;\ninteger g = 2;\ninteger k = h + g;\n"
        "string s = t + \"x\";\nstring t = \"y\";\nlist a = b;\nlist b = [1];\n"
        "default { state_entry() { llOwnerSay((string)h + \" \" + (string)g + \" \" + (string)k + \" \" + s + \" \" + "
        "t + \" \" + (string)llGetListLength(a) + (string)llGetListLength(b)); } }\n");
    check(forward == "0 2 2 x y 01", "Halcyon: a global read before its declaration holds its default value",
          forward);

    // rule 3: a function, a variable and a state may share a name
    const std::string shared = run_and_say(
        "key StopSound = \"k1\";\n"
        "StopSound() { llOwnerSay(\"fn \" + (string)StopSound); }\n"
        "integer two = 5;\n"
        "default { state_entry() { StopSound(); StopSound = \"k2\"; StopSound(); state two; } }\n"
        "state two { state_entry() { llOwnerSay((string)two); } }\n");
    check(shared == "fn k1 / fn k2 / 5", "Halcyon: a function, a global and a state may share a name", shared);
    {
        CompileResult r = compile("integer F;\nF() { }\ndefault { state_entry() { F(1); } }\n");
        std::string message;
        for (const auto& d : r.diagnostics) {
            if (d.is_error) message = d.message;
        }
        check(compiles("integer F;\nF() { }\ndefault { state_entry() { F(); } }\n") && !r.ok &&
                  message.find("F") != std::string::npos,
              "Halcyon: an error in a function that shares a global's name names it", message);
    }

    // rule 4: one `)` too many before the `;` of an assignment statement
    const std::string stray = run_and_say(says(
        "", {"integer a = 1; a = (a + 2)); vector v; v.x = 1.5); list L; L += [1]); if (a) a += 1);",
             "do a = a - 1); while (a > 10);",
             "llOwnerSay((string)a + \" \" + (string)v.x + \" \" + (string)llGetListLength(L));"}));
    check(stray == "3 1.500000 1", "Halcyon: an extra `)` before `;` in an assignment is ignored", stray);
    check(compiles(says("", {"integer a; a = (a + 1));"})) &&
              !compiles(says("", {"llOwnerSay(\"a\"));"})) &&
              !compiles(says("", {"integer a = 1);"})) &&
              !compiles(says("", {"integer a; a = a)));"})),
          "Halcyon: an extra `)` is still refused in calls, declarations, and twice");

    // a state change in a function ends the event (ExecutionScheduler.cs interp_OnStateChg)
    const std::string fn_state_src =
        "integer Go() { llOwnerSay(\"a\"); state two; llOwnerSay(\"b\"); return 5; }\n"
        "default { state_entry() { integer x = Go(); llOwnerSay(\"c\"); } state_exit() { llOwnerSay(\"exit\"); } }\n"
        "state two { state_entry() { llOwnerSay(\"two\"); } }\n";
    const std::string fn_state = run_and_say(fn_state_src);
    check(fn_state == "a / exit / two", "Halcyon: `state x;` in a function ends the event, then exit and entry",
          fn_state);

    // rule 6: a cast takes a whole unary expression (Phlox bytecode: fneg before icast)
    const std::string casts = run_and_say(says(
        "float f = 1.5; integer i = 2;",
        {"llOwnerSay((string)((integer)-(f + 1.0)) + \" \" + (string)((integer)-f * 2) + \" \" + (string)((integer)!i) + "
         "\" \" + (string)((integer)~i) + \" \" + (string)-i + \" \" + (string)((integer)(float)-f) + \" \" + "
         "(string)(integer)\"5\" + \" \" + (string)((integer)-(f) + 1));"}));
    check(casts == "-2 -2 0 -3 -2 -1 5 0", "Halcyon: a cast takes a prefix operator or another cast", casts);

    // rule 5: decimal literals outside 32 bits are -1 in InWorldz; hex wraps in both
    const std::string literals_src = says(
        "", {"integer a = 3000000000; integer b = -2147483649; integer c = 0x80000000; integer d = 2147483648;",
             "llOwnerSay((string)a + \" \" + (string)b + \" \" + (string)c + \" \" + (string)d);"});
    const std::string sl_literals = run_and_say(literals_src);
    check(sl_literals == "-1294967296 2147483647 -2147483648 -2147483648",
          "decimal literals up to 4294967295 wrap, as in Second Life", sl_literals);

    // Halcyon's compiler tables: its own constants and signatures
    const std::string tables = run_and_say(says(
        "", {"llOwnerSay((string)IW_REZ_OK + \" \" + (string)IW_POWER_MANAGE_PASSES + \" \" + "
             "(string)VEHICLE_INVERTED_BANKING_MODIFIER + \" \" + (string)IWERR_NOTFOUND);"}));
    check(tables == "0 -1 11006 7", "Halcyon: the constants only its compiler defines", tables);
    check(compiles(says("", {"integer r = iwDeliverInventory(1, NULL_KEY, \"x\");",
                             "key k = iwGetLastOwner(); if (iwAvatarOnLink(1)) { }"})),
          "Halcyon: iw* signatures are its compiler's (iwDeliverInventory returns an integer)");
}

}  // namespace

void run_semantics() {
    integers();
    division_by_zero();
    floats();
    casts();
    list_typing();
    list_ops();
    strings();
    encodings();
    rsa_functions();
    math_functions();
    vectors_rotations();
    states();
    jumps();
    big_functions();
    evaluation_order();
    functions_globals();
    host_and_clocks();
    limits();
    compile_errors();
    dialects();
    inworldz();
    halcyon_rules();
}

}  // namespace compat
