#include "ime_core.h"
#include <cassert>
#include <iostream>
#include <set>
using namespace metalio_plus_ime;

int main() {
    std::set<std::string> chars;
    for (std::size_t i = 0; i < kPinyinCount; ++i) {
        auto found = Candidates(kPinyin[i].syllable);
        assert(!found.empty() && IsPrefix(kPinyin[i].syllable));
        std::string joined;
        for (const auto& c : found) { joined += c; chars.insert(c); assert(ValidUtf8(c)); }
        assert(joined == kPinyin[i].candidates_utf8);
    }
    assert(chars.size() == 4262 && kPinyinCount == 402);
    assert(Candidates("si") == Candidates("SI"));
    assert(Candidates("shi").front() == u8"是");
    assert(Candidates("chi").front() == u8"吃");
    assert(Candidates("si").front() == u8"死"); // current data, not a quality claim
    assert(Candidates("nihao").empty() && !IsPrefix("nihao"));
    assert(Candidates("sh").empty() && IsPrefix("sh"));
    assert(Candidates("shi4").empty() && Candidates("").empty() && Candidates(u8"ü").empty());
    for (const auto& item : {std::make_pair("chong", u8"重"), std::make_pair("zhong", u8"重"),
                            std::make_pair("hang", u8"行"), std::make_pair("xing", u8"行"),
                            std::make_pair("lv", u8"绿"), std::make_pair("nv", u8"女"),
                            std::make_pair("lve", u8"略")}) {
        auto list = Candidates(item.first);
        assert(std::find(list.begin(), list.end(), item.second) != list.end());
    }
    Editor limited(4, true);
    assert(limited.Reset(u8"中", 3) == Result::Changed);
    assert(limited.TypeLetter('S') == Result::Changed && limited.TypeLetter('I') == Result::Changed);
    const auto shown = limited.Revision();
    assert(limited.Enter(shown) == Result::ByteLimit);
    assert(limited.Text() == u8"中" && limited.Composition() == "si" && limited.Cursor() == 3);
    assert(limited.ClearComposition() == Result::Changed && limited.Text() == u8"中");
    assert(limited.InsertLiteral("A") == Result::Changed);
    assert(limited.Backspace() == Result::Changed && limited.Text() == u8"中");
    assert(limited.Backspace() == Result::Changed && limited.Text().empty());

    Editor editor(100, true);
    editor.TypeLetter('s'); editor.TypeLetter('h');
    const auto old = editor.Revision();
    editor.TypeLetter('i');
    assert(editor.SelectVisible(0, old) == Result::StaleView && editor.Text().empty());
    const auto first_page = editor.Revision();
    assert(editor.AdvancePage(1, first_page) == Result::Changed);
    auto second = editor.VisibleCandidates();
    assert(!second.empty());
    assert(editor.SelectVisible(0, first_page) == Result::StaleView);
    assert(editor.Space(editor.Revision()) == Result::Changed && editor.Text() == second.front());
    assert(editor.Digit('1', first_page) == Result::StaleView); // no stale selection becomes a digit
    editor.TypeLetter('a');
    assert(editor.SetMode(Mode::Latin) == Result::CompositionPending);
    assert(editor.ClearComposition() == Result::Changed && editor.Text() == second.front());
    assert(editor.SetMode(Mode::Latin) == Result::Changed);
    assert(editor.TypeLetter('A') == Result::Changed);

    Editor single(20, false);
    single.TypeLetter('n'); single.TypeLetter('i');
    const auto with_pinyin = single.Revision();
    assert(single.Enter(with_pinyin) == Result::Changed && single.Text() == "ni");
    assert(single.Enter(with_pinyin) == Result::StaleView);
    assert(single.Enter(single.Revision()) == Result::Confirm);
    assert(single.InsertLiteral("\n") == Result::Invalid);

    Editor unicode(20, true);
    assert(unicode.InsertLiteral(u8"中😀A") == Result::Changed);
    assert(unicode.MoveCursor(1, unicode.Revision()) == Result::Invalid);
    assert(unicode.MoveCursor(7, unicode.Revision()) == Result::Changed);
    assert(unicode.Backspace() == Result::Changed && unicode.Text() == u8"中A");
    assert(unicode.MoveCursor(0, unicode.Revision()) == Result::Changed);
    assert(unicode.Backspace() == Result::Unchanged);
    assert(!ValidUtf8(std::string("\xC0\xAF", 2)));
    assert(!ValidUtf8(std::string("\xED\xA0\x80", 3)));
    assert(!ValidUtf8(std::string("\xF4\x90\x80\x80", 4)));
    assert(!ValidUtf8(std::string("\xE4\xB8", 2)));
    assert(!ValidUtf8(std::string(1, '\0')));
    const auto before = unicode.Text();
    assert(unicode.InsertLiteral(std::string("\xE4\xB8", 2)) == Result::Invalid && unicode.Text() == before);
    std::cout << "PASS: 402 rows/4262 characters, UTF-8, atomic limits, stale candidates, pages, dynamic enter\n";
}
