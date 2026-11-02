#include <gtest/gtest.h>

#include <set>

#include "forma/editor/language_registry.h"

using namespace forma::editor;

TEST(Languages, CoversAWideRange)
{
    LanguageRegistry registry;
    EXPECT_GE(registry.all().size(), 150u);
    for (const char* id : {"cpp", "c", "swift", "kotlin", "java", "csharp", "javascript", "typescript", "python", "html", "css", "qml", "rust", "go", "dart", "ruby", "php", "lua", "bash", "powershell", "sql", "zig", "nim", "haskell", "ocaml", "elixir", "erlang", "scala", "fsharp", "r", "julia", "fortran", "cobol", "ada", "verilog", "vhdl", "asm", "markdown", "json", "yaml", "toml", "xml", "cmake", "dockerfile"})
        EXPECT_NE(registry.find(id), nullptr) << id;
}

TEST(Languages, DetectsFromPathNameAndShebang)
{
    LanguageRegistry registry;
    EXPECT_EQ(registry.forPath("src/main.cpp")->id, "cpp");
    EXPECT_EQ(registry.forPath("C:\\work\\App.KT")->id, "kotlin");
    EXPECT_EQ(registry.forPath("CMakeLists.txt")->id, "cmake");
    EXPECT_EQ(registry.forPath("Dockerfile")->id, "dockerfile");
    EXPECT_EQ(registry.forPath("deep/path/Makefile")->id, "makefile");
    EXPECT_EQ(registry.forPath("no_extension"), nullptr);
    EXPECT_EQ(registry.forShebang("#!/usr/bin/env python3")->id, "python");
    EXPECT_EQ(registry.forShebang("#!/bin/bash -e")->id, "bash");
    EXPECT_EQ(registry.forShebang("not a shebang"), nullptr);
}

TEST(Languages, EveryDefinitionUsesARealLexilaLexer)
{
    static const std::set<std::string> lexers = {
        "COBOL", "DMAP", "DMIS", "PL/M", "SML", "TACL", "TAL", "a68k", "abaqus", "abl", "ada", "apdl", "as", "asciidoc", "asm", "asn1", "asy", "au3", "ave", "avs", "baan", "bash", "batch", "bib", "blitzbasic", "bullant", "caml", "cil", "clarion", "clarionnocase", "cmake", "coffeescript", "conf", "cpp", "cppnocase", "csound", "css", "d", "dart", "dataflex", "diff", "edifact", "eiffel", "eiffelkw", "errorlist", "escript", "escseq", "f77", "fcST", "flagship", "forth", "fortran", "freebasic", "fsharp", "gdscript", "gui4cli", "haskell", "hollywood", "hypertext", "ihex", "indent", "inno", "json", "julia", "kix", "kvirc", "latex", "lisp", "literatehaskell", "lot", "lout", "lua", "magiksf", "makefile", "markdown", "matlab", "maxima", "metapost", "mmixal", "modula", "mssql", "mysql", "nim", "nimrod", "nix", "nncrontab", "nsis", "null", "octave", "opal", "oscript", "pascal", "perl", "phpscript", "po", "pov", "powerbasic", "powerpro", "powershell", "props", "ps", "purebasic", "python", "r", "raku", "rebol", "ruby", "rust", "sas", "scriptol", "sinex", "smalltalk", "sorcins", "specman", "spice", "sql", "srec", "stata", "tads3", "tcl", "tcmd", "tehex", "tex", "toml", "troff", "txt2tags", "vb", "vbscript", "verilog", "vhdl", "visualprolog", "x12", "xml", "yaml", "zig"};
    LanguageRegistry registry;
    for (const auto& language : registry.all())
        EXPECT_EQ(lexers.count(language.lexer), 1u) << language.id << " uses " << language.lexer;
}

TEST(Languages, PluginsCanAddAndOverride)
{
    LanguageRegistry registry;
    ASSERT_TRUE(registry.addFile(R"({"id":"brandnew","name":"Brand New","extensions":["bnw"],"lexer":"cpp","comment":"//"})", "plugin:x").ok());
    const auto* language = registry.forPath("a.bnw");
    ASSERT_NE(language, nullptr);
    EXPECT_EQ(language->name, "Brand New");
    EXPECT_EQ(language->source, "plugin:x");
    EXPECT_FALSE(registry.addFile("{ broken", "plugin:x").ok());
    EXPECT_FALSE(registry.addFile(R"({"name":"no id"})", "plugin:x").ok());
}
