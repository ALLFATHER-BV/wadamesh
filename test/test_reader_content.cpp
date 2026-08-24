// SPDX-License-Identifier: GPL-3.0-or-later

#include <assert.h>
#include <string.h>

#include "ui-touch/ReaderContent.h"

int main() {
  static const char html[] =
      "<!doctype html><html><body><h1>Bookmarks</h1>"
      "<p><a href='https://lite.cnn.com'>CNN</a></p>"
      "<p><a href='news.htm'>Local news</a> &amp; weather</p>"
      "<script>12345</script></body></html>";
  char text[256];
  ReaderContent::Link links[4] = {};
  size_t link_count = 0;
  const size_t text_len = ReaderContent::htmlToText(
      html, strlen(html), text, sizeof text, "sd:/home.htm",
      links, 4, &link_count);

  assert(text_len == strlen(text));
  assert(strstr(text, "Bookmarks") != nullptr);
  assert(strstr(text, "CNN") != nullptr);
  assert(strstr(text, "Local news & weather") != nullptr);
  assert(strstr(text, "12345") == nullptr);
  assert(link_count == 2);
  assert(strcmp(links[0].href, "https://lite.cnn.com") == 0);
  assert(strcmp(links[1].href, "sd:/news.htm") == 0);
  assert(strncmp(text + links[0].start, "CNN", links[0].end - links[0].start) == 0);
  assert(strncmp(text + links[1].start, "Local news", links[1].end - links[1].start) == 0);

  char resolved[240];
  assert(ReaderContent::resolveUrl("sd:/bookmarks/home.htm", "../index.htm",
                                   resolved, sizeof resolved));
  assert(strcmp(resolved, "sd:/bookmarks/../index.htm") == 0);
  assert(ReaderContent::resolveUrl("sd:/home.htm", "//text.npr.org",
                                   resolved, sizeof resolved));
  assert(strcmp(resolved, "https://text.npr.org") == 0);
  assert(!ReaderContent::resolveUrl("sd:/home.htm", "javascript:alert(1)",
                                    resolved, sizeof resolved));

  static const char attribute_html[] =
      "<a data-href='sd:/wrong.htm' xhref='sd:/also-wrong.htm' href='right.htm'>Right</a>";
  link_count = 0;
  ReaderContent::htmlToText(attribute_html, strlen(attribute_html), text, sizeof text,
                            "sd:/home.htm", links, 4, &link_count);
  assert(link_count == 1);
  assert(strcmp(links[0].href, "sd:/right.htm") == 0);
  return 0;
}