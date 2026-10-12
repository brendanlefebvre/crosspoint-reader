# KOSync div/span positions

Chapters I–III of *Alice's Adventures in Wonderland* recoded in the shape Calibre gives Kindle/MOBI-derived books (CrossPoint #3665): every paragraph is a `<div><span>` with no `<p>` around the text, each chapter opens with an empty `<p id="filepos…">` anchor, and italics are `<span class="italic">`. A paragraph with italics in mid-sentence has several text runs inside one `<span>`, which KOReader numbers `text()[1]`, `text()[2]`, and so on. Each chapter fills several pages at any common font size, so a page can start partway through a chapter.

On a device with KOReader Sync configured, open chapter I, turn three or four pages in, and upload progress. The pushed XPath should name a `div[N]/span[1]/…/text()[K]` inside the chapter, never `p[1]` or bare `body`. A KOReader pull of the same file should land on the same page. Pages that start just after italic text (*very*, *never*, *through*) exercise `text()[2]` and later. `test/koreader_xpath_resolver` resolves offsets in `OEBPS/chapter01.xhtml` directly.

Text: Lewis Carroll, *Alice's Adventures in Wonderland* (1865), public domain, transcribed from Project Gutenberg eBook #11 with its header and license removed.

Package the source files with `mimetype` first and uncompressed; all paths are relative to this folder. The packaged fixture is `test/epubs/test_kosync_div_span.epub`.
