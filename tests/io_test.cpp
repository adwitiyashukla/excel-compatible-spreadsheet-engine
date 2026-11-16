#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "deflate.h"
#include "test.h"
#include "util.h"
#include "xlsx.h"
#include "xml.h"
#include "zip.h"

using namespace sheet;
using test::Book;

namespace {

std::string FromHex(std::string_view hex) {
  std::string out;
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    out.push_back(static_cast<char>(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16)));
  }
  return out;
}

std::string Pseudo(size_t n, uint64_t seed, int alphabet) {
  std::string out;
  uint64_t s = seed;
  for (size_t i = 0; i < n; ++i) {
    s += 0x9E3779B97F4A7C15ULL;
    out.push_back(static_cast<char>(Mix64(s) % static_cast<uint64_t>(alphabet)));
  }
  return out;
}

std::string TempPath(const std::string& name) {
  return (std::filesystem::temp_directory_path() / ("sheet_io_test_" + name)).string();
}

}

TEST(Crc32KnownValues) {
  CHECK_EQ(Crc32("123456789", 9), 0xCBF43926u);
  CHECK_EQ(Crc32("", 0), 0u);
  std::string text;
  for (int i = 0; i < 40; ++i) {
    text += "row " + std::to_string(i) + ",value " + std::to_string(i * i % 97) + ",label item-" +
            std::to_string(i % 13) + ";";
  }
  CHECK_EQ(Crc32(text.data(), text.size()), 0x34b5cfc8u);
  uint32_t part = Crc32(text.data(), 100);
  CHECK_EQ(Crc32(text.data() + 100, text.size() - 100, part), 0x34b5cfc8u);
}

TEST(InflatesStreamsFromZlib) {
  std::string out;
  CHECK(Inflate(FromHex("cb48cdc9c957c8402775145252d372124b5215caf38b72521401"), out));
  CHECK_EQ(out, std::string("hello hello hello hello, deflate world!"));
  CHECK(Inflate(FromHex("012700d8ff68656c6c6f2068656c6c6f2068656c6c6f2068656c6c6f2c206465666c617465"
                        "20776f726c6421"),
                out));
  CHECK_EQ(out, std::string("hello hello hello hello, deflate world!"));
  CHECK(Inflate(FromHex("0300"), out));
  CHECK(out.empty());
  const char* dynamic =
      "6dd35b6a04211085e1adcc0212e8aaa3a592d54ca01f021d02432edbcf607b1aa5eac9871f6debc37e7cfdddb697"
      "dffbf1b33fd7e"
      "3febe1fb78feffdf3757b7b3c938c2473929e74a43427ed0923b539a1a7c4036d6ea9b73c9ae6b9e5de6c342cfba"
      "cb7c29b2cdf2bb"
      "dd5d16cb966edad8d5697e9da393851b04c3e54c8a2cba972c2c82553967ada08718a06dce451ef2de4817871a18"
      "fa9371702b5ec"
      "d185428057171295cdb32b8944bdbb9228c3c32b859a797925504a01bd52a84940af344a08e895482d797a2552de"
      "82b74ea4eae995"
      "46963d3d68a4c5d38346b57a7ad028274f0f1a69400f12d5e6e941210b1e3daebf2c07f4a09044f4b8882ca0c785"
      "642bfd3f";
  CHECK(Inflate(FromHex(dynamic), out));
  CHECK_EQ(out.size(), 1152u);
  CHECK_EQ(Crc32(out.data(), out.size()), 0x34b5cfc8u);
}

TEST(InflateRejectsBadInput) {
  std::string out;
  CHECK(!Inflate(FromHex("07"), out));
  CHECK(!Inflate(FromHex("cb48cdc9c957c840"), out));
  CHECK(!Inflate(FromHex("012700d8ff6865"), out));
  CHECK(!Inflate(FromHex("010500faff"), out));
  std::string big = Deflate(std::string(1 << 20, 'x'));
  CHECK(Inflate(big, out));
  CHECK_EQ(out.size(), static_cast<size_t>(1 << 20));
  CHECK(!Inflate(big, out, 0, 1000));
  for (size_t cut = 1; cut + 1 < big.size(); cut += 7) {
    std::string piece = big.substr(0, cut);
    std::string sink;
    Inflate(piece, sink);
  }
}

TEST(DeflateRoundTrips) {
  std::vector<std::string> inputs = {
      "",
      "a",
      "abcabcabcabcabcabcabcabcabc",
      std::string(70000, 'z'),
      Pseudo(5000, 1, 256),
      Pseudo(200000, 2, 4),
      Pseudo(100000, 3, 26),
  };
  std::string text;
  for (int i = 0; i < 5000; ++i)
    text += "<c r=\"A" + std::to_string(i) + "\"><v>" + std::to_string(i * 3) + "</v></c>";
  inputs.push_back(text);
  for (const std::string& in : inputs) {
    for (int level : {0, 1, 6, 9}) {
      std::string packed = Deflate(in, level);
      std::string back;
      CHECK(Inflate(packed, back, in.size()));
      CHECK(back == in);
      if (level > 0 && in.size() > 1000 && in == text) CHECK(packed.size() * 4 < in.size());
    }
  }
}

TEST(ZipRoundTrip) {
  ZipWriter w;
  w.Add("a.txt", "hello");
  w.Add("dir/b.xml", std::string(10000, 'b'));
  w.Add("empty", "");
  w.Add("raw.bin", Pseudo(3000, 9, 256), 0);
  std::string data = w.Finish();
  ZipReader r;
  std::string error;
  CHECK(r.Open(data, &error));
  CHECK(r.Has("a.txt"));
  CHECK(r.Has("dir/b.xml"));
  CHECK(!r.Has("missing"));
  std::string out;
  CHECK(r.Read("a.txt", out, &error));
  CHECK_EQ(out, std::string("hello"));
  CHECK(r.Read("dir/b.xml", out, &error));
  CHECK(out == std::string(10000, 'b'));
  CHECK(r.Read("empty", out, &error));
  CHECK(out.empty());
  CHECK(r.Read("raw.bin", out, &error));
  CHECK(out == Pseudo(3000, 9, 256));
  CHECK(!r.Read("missing", out, &error));
  CHECK_EQ(r.Names().size(), 4u);
  ZipReader bad;
  CHECK(!bad.Open("not a zip file at all", &error));
  std::string corrupt = data;
  corrupt[corrupt.size() / 3] ^= 0x55;
  ZipReader c;
  if (c.Open(corrupt, &error)) {
    for (const std::string& n : c.Names()) c.Read(n, out, &error);
  }
}

TEST(XmlReaderEvents) {
  std::string doc =
      "<?xml version=\"1.0\"?><!-- note --><root xmlns:x=\"urn:x\" a=\"1\" b='two'>"
      "<x:item id=\"&lt;3&gt;\">A &amp; B &#65;&#x42;</x:item><empty/><![CDATA[<raw>]]>"
      "<deep><deeper k=\"v\"><skip>ignored</skip></deeper></deep></root>";
  XmlReader x(doc);
  std::vector<std::string> trace;
  while (true) {
    XmlReader::Event e = x.Next();
    if (e == XmlReader::kEof || e == XmlReader::kError) {
      CHECK(e == XmlReader::kEof);
      break;
    }
    if (e == XmlReader::kStart) {
      std::string name(x.Name());
      std::string v;
      if (x.Attr("id", v)) name += "[" + v + "]";
      if (x.Attr("b", v)) name += "[" + v + "]";
      trace.push_back("<" + name);
      if (x.Name() == "deeper") x.Skip();
    } else if (e == XmlReader::kEnd) {
      trace.push_back("/" + std::string(x.Name()));
    } else if (!x.Text().empty()) {
      trace.push_back("t:" + x.Text());
    }
  }
  std::string joined;
  for (const std::string& t : trace) joined += t + "|";
  CHECK_EQ(joined, std::string("<root[two]|<item[<3>]|t:A & B "
                               "AB|/item|<empty|/empty|t:<raw>|<deep|<deeper|/deep|/root|"));
  CHECK_EQ(XmlDecode("&quot;x&apos; &amp;amp;"), std::string("\"x' &amp;"));
  CHECK_EQ(XmlEscape("<a & \"b\">"), std::string("&lt;a &amp; &quot;b&quot;&gt;"));
  XmlReader broken("<a><b></a>");
  int guard = 0;
  while (guard++ < 100) {
    XmlReader::Event e = broken.Next();
    if (e == XmlReader::kEof || e == XmlReader::kError) break;
  }
  CHECK(guard < 100);
}

TEST(XlsxRoundTrip) {
  Book b;
  int s2 = b.wb.AddSheet("Second & More");
  b.Set("A1", "42");
  b.Set("A2", "'0012");
  b.Set("A3", "TRUE");
  b.Set("A4", "#N/A");
  b.Set("A5", "  spaced <tag> & \"quotes\"  ");
  b.Set("A6", "\xE2\x82\xAC unicode \xF0\x9F\x98\x82");
  b.Set("A7", "3/14/2026");
  b.Set("B1", "=A1*2");
  b.Set("B2", "=LEN(A2)");
  b.Set("B3", "=IF(A3,\"yes\",\"no\")");
  b.Set("B4", "=ISNA(A4)");
  b.Set("B5", "=XLOOKUP(42,A1:A7,A1:A7)");
  b.Set("B6", "=SUM('Second & More'!A1:A3)");
  b.Set("A1", "5", s2);
  b.Set("A2", "=A1+1", s2);
  b.Set("A3", "=Rate*100", s2);
  CHECK(b.wb.DefineName("Rate", "Sheet1!$A$1"));
  CHECK(b.wb.SetArrayFormula(b.sheet, Area{b.sheet, 0, 3, 1, 3}, "=A1:A2&\"!\""));
  b.wb.RecalcAll();
  std::string path = TempPath("roundtrip.xlsx");
  std::string error;
  CHECK(SaveXlsx(b.wb, path, &error));
  Workbook back;
  LoadReport rep;
  CHECK(LoadWorkbook(path, back, rep, &error));
  CHECK_EQ(back.SheetCount(), 2);
  CHECK_EQ(back.SheetName(1), std::string("Second & More"));
  CHECK_EQ(rep.failed, 0u);
  back.RecalcAll();
  for (int s = 0; s < 2; ++s) {
    for (int r = 0; r < 8; ++r) {
      for (int c = 0; c < 4; ++c) {
        CHECK(b.wb.Get(s, r, c).SameAs(back.Get(s, r, c)));
        CHECK_EQ(b.wb.FormulaText(s, r, c), back.FormulaText(s, r, c));
      }
    }
  }
  Area g;
  CHECK(back.ArrayGroup(0, 1, 3, g));
  CHECK_EQ(test::Display(back.Get(0, 4, 1)), std::string("42"));
  CHECK_EQ(test::Display(back.Get(1, 2, 0)), std::string("4200"));
  std::filesystem::remove(path);
}

TEST(LoadsFlatOpenDocument) {
  std::string fods =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      "<office:document xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
      "xmlns:table=\"urn:oasis:names:tc:opendocument:xmlns:table:1.0\" "
      "xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\">"
      "<office:body><office:spreadsheet>"
      "<table:table table:name=\"Data\">"
      "<table:table-row>"
      "<table:table-cell office:value-type=\"float\" "
      "office:value=\"2\"><text:p>2</text:p></table:table-cell>"
      "<table:table-cell office:value-type=\"string\"><text:p>two  "
      "words</text:p></table:table-cell>"
      "<table:table-cell table:formula=\"of:=[.A1]*[.A2]\" office:value-type=\"float\" "
      "office:value=\"6\">"
      "<text:p>6</text:p></table:table-cell>"
      "</table:table-row>"
      "<table:table-row table:number-rows-repeated=\"2\">"
      "<table:table-cell office:value-type=\"float\" "
      "office:value=\"3\"><text:p>3</text:p></table:table-cell>"
      "<table:table-cell office:value-type=\"string\"><text:p>a<text:s "
      "text:c=\"2\"/>b</text:p></table:table-cell>"
      "<table:table-cell table:formula=\"of:=SUM([.A1:.A3]);\" office:value-type=\"float\" "
      "office:value=\"8\">"
      "<text:p>8</text:p></table:table-cell>"
      "</table:table-row>"
      "<table:table-row>"
      "<table:table-cell table:formula=\"of:=Total*2\" office:value-type=\"float\" "
      "office:value=\"16\">"
      "<text:p>16</text:p></table:table-cell>"
      "</table:table-row>"
      "</table:table>"
      "<table:named-expressions>"
      "<table:named-range table:name=\"Total\" table:base-cell-address=\"$Data.$A$1\" "
      "table:cell-range-address=\"$Data.$C$2\"/>"
      "</table:named-expressions>"
      "</office:spreadsheet></office:body></office:document>";
  std::string path = TempPath("tiny.fods");
  CHECK(WriteFile(path, fods));
  Workbook wb;
  LoadReport rep;
  std::string error;
  CHECK(LoadWorkbook(path, wb, rep, &error));
  wb.RecalcAll();
  CHECK_EQ(wb.SheetName(0), std::string("Data"));
  CHECK_EQ(test::Display(wb.Get(0, 0, 1)), std::string("two words"));
  CHECK_EQ(test::Display(wb.Get(0, 1, 1)), std::string("a  b"));
  CHECK_EQ(test::Display(wb.Get(0, 0, 2)), std::string("6"));
  CHECK_EQ(test::Display(wb.Get(0, 2, 2)), std::string("8"));
  CHECK_EQ(test::Display(wb.Get(0, 3, 0)), std::string("16"));
  CHECK_EQ(wb.FormulaText(0, 0, 2), std::string("=A1*A2"));
  std::filesystem::remove(path);
}

TEST(CsvExportQuotes) {
  Book b;
  b.Set("A1", "plain");
  b.Set("B1", "has,comma");
  b.Set("C1", "has \"quote\"");
  b.Set("A2", "1.5");
  b.Set("C2", "=A2*2");
  b.wb.RecalcAll();
  std::string csv = SheetCsv(b.wb, b.sheet);
  CHECK_EQ(csv, std::string("plain,\"has,comma\",\"has \"\"quote\"\"\"\n1.5,,3\n"));
}

TEST_MAIN
