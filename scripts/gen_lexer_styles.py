import glob
import json
import os
import re
import sys

lexilla = sys.argv[1]
output = sys.argv[2]

with open(os.path.join(lexilla, "include", "LexicalStyles.iface"), encoding="latin-1") as f:
    iface = f.read()

lexer_prefixes = {}
for line in re.findall(r"^lex \w+=(.*)$", iface, re.M):
    parts = line.split()
    clex = parts[0]
    lexer_prefixes.setdefault(clex, []).extend(parts[1:])

style_values = {name: int(value) for name, value in re.findall(r"^val (SCE_\w+)=(\d+)", iface, re.M)}

RULES = [
    ("error", r"ERROR|BAD|INVALID|UNCLOSED|DELETED|REMOVED|STRINGEOL|UNTERMINATED"),
    ("success", r"ADDED|INSERTED"),
    ("comment", r"COMMENT|_DOC|DOCSTRING|_POD|_NOTE|REM$"),
    ("number", r"NUMBER|_NUM|NUMERIC|_HEX|FLOAT|INTEGER|_INT$|BINARY|OCTAL|DECIMAL"),
    ("string", r"STRING|_STR|CHAR|TRIPLE|HEREDOC|HERE_|QUOTE|VERBATIM|_BACKTICKS|REGEX|REGEXP|LITERAL|_VALUE|ATTVALUE|DOUBLESTRING|SINGLESTRING"),
    ("preprocessor", r"PREPROC|DIRECTIVE|MACRO|PRAGMA|INCLUDE|DEFINE|SCRIPT|_XMLSTART|_XMLEND|_CDATA|_SECTION|ANNOTATION|DECORATOR"),
    ("type", r"WORD2|KEYWORD2|KW2|_TYPE|TYPE_|CLASS|STRUCT|_ATTRIBUTE|ATTRIBUTEUNKNOWN|_SPECIAL|_BUILTIN|BUILTIN|_INTRINSIC|LIFETIME|TAGEND|WORD3|WORD4|_KW3|_KW4"),
    ("keyword", r"WORD|KEYWORD|_KW|INSTRUCTION|RESERVED|_COMMAND|_TAG|_ELEMENT|STATEMENT|_DIRECT|_HEADER|_H[1-6]$|STRONG|EM[12]$"),
    ("function", r"FUNCTION|METHOD|DEFNAME|FUNC|_PROC|_LABEL|_TARGET"),
    ("operator", r"OPERATOR|_OPER|_OP$|DELIMITER|PUNCT|BRACE|BRACKET|_OP2"),
]


def categorize(clex):
    table = {}
    for prefix in lexer_prefixes.get(clex, []):
        for name, number in style_values.items():
            if not name.startswith(prefix) or number in table:
                continue
            for category, pattern in RULES:
                if re.search(pattern, name[len(prefix) - 1:]):
                    table[number] = category
                    break
    return table


result = {}
for path in sorted(glob.glob(os.path.join(lexilla, "lexers", "*.cxx"))):
    with open(path, encoding="latin-1") as f:
        source = f.read()
    for clex, name in re.findall(r"LexerModule\s+\w+\s*\(\s*(SCLEX_\w+)\s*,[^;]*?\"([^\"]+)\"", source, re.S):
        table = categorize(clex)
        if table:
            result[name] = {str(k): v for k, v in sorted(table.items())}

for alias, source in {"cppnocase": "cpp", "phpscript": "hypertext", "literatehaskell": "haskell"}.items():
    if source in result:
        result[alias] = result[source]

with open(output, "w", newline="\n") as f:
    json.dump(result, f, indent=0, sort_keys=True)
    f.write("\n")
print(len(result), "lexers mapped")
