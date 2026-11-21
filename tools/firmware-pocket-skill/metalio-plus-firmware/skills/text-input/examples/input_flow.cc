// Build alongside the same ime_core.h and generated pinyin_table.h used by the app.
// This headless example proves input flow; it is not a hardware-rendering test.
#include "ime_core.h"
#include <iostream>

int main() {
    metalio_plus_ime::Editor editor(128, true);
    editor.TypeLetter('n');
    editor.TypeLetter('i');
    // Real app: paint text + candidates from a coherent snapshot. Only record its
    // Revision after that physical frame succeeds. Do not mark an enqueued frame shown.
    const auto displayed_revision = editor.Revision();
    const auto result = editor.SelectVisible(0, displayed_revision);
    if (result != metalio_plus_ime::Result::Changed) { return 1; }
    // Real app: persist only Text() through its selected storage adapter. Pending
    // Composition() must be explicitly committed/cancelled before leaving the field.
    std::cout << editor.Text() << '\n';
    return 0;
}
