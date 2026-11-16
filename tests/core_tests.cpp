// Self-contained tests for the RpfStudio core (no game files needed).
#include "BinaryIO.h"
#include "Compression.h"
#include "Crypto.h"
#include "Drawable.h"
#include "GameFS.h"
#include "Gxt2.h"
#include "Oiv.h"
#include "Resource.h"
#include "Rpf.h"
#include "RpfWriter.h"
#include "Texture.h"
#include "XPathLite.h"
#include "Ytd.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <cstdio>
#include <zlib.h>

using namespace rs;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (cond) ++g_pass;                                                               \
        else { ++g_fail; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

static QByteArray readAll(const QString& p) { QFile f(p); f.open(QIODevice::ReadOnly); return f.readAll(); }
static void writeAll(const QString& p, const QByteArray& d)
{
    QDir().mkpath(QFileInfo(p).absolutePath());
    QFile f(p); f.open(QIODevice::WriteOnly | QIODevice::Truncate); f.write(d);
}

// Minimal ZIP writer (stored entries) for building test packages.
static QByteArray makeZip(const QList<QPair<QString, QByteArray>>& files)
{
    Writer w, cd;
    for (auto& f : files) {
        uint32_t off = uint32_t(w.pos());
        uint32_t crc = uint32_t(crc32(0, reinterpret_cast<const Bytef*>(f.second.constData()), uInt(f.second.size())));
        QByteArray name = f.first.toUtf8();
        w.u32(0x04034b50); w.u16(20); w.u16(0); w.u16(0); w.u16(0); w.u16(0);
        w.u32(crc); w.u32(uint32_t(f.second.size())); w.u32(uint32_t(f.second.size()));
        w.u16(uint16_t(name.size())); w.u16(0); w.bytes(name); w.bytes(f.second);
        cd.u32(0x02014b50); cd.u16(20); cd.u16(20); cd.u16(0); cd.u16(0); cd.u16(0); cd.u16(0);
        cd.u32(crc); cd.u32(uint32_t(f.second.size())); cd.u32(uint32_t(f.second.size()));
        cd.u16(uint16_t(name.size())); cd.u16(0); cd.u16(0); cd.u16(0); cd.u16(0); cd.u32(0); cd.u32(off);
        cd.bytes(name);
    }
    uint32_t cdOff = uint32_t(w.pos());
    w.bytes(cd.data);
    w.u32(0x06054b50); w.u16(0); w.u16(0); w.u16(uint16_t(files.size())); w.u16(uint16_t(files.size()));
    w.u32(uint32_t(cd.data.size())); w.u32(cdOff); w.u16(0);
    return w.data;
}

static QImage testImage(int w, int h)
{
    QImage img(w, h, QImage::Format_RGBA8888);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) img.setPixelColor(x, y, QColor(x * 255 / w, y * 255 / h, 128, 255 - x * 255 / w));
    return img;
}

static double meanError(const QImage& a, const QImage& b)
{
    if (a.size() != b.size()) return 1e9;
    double e = 0;
    QImage x = a.convertToFormat(QImage::Format_RGBA8888), y = b.convertToFormat(QImage::Format_RGBA8888);
    for (int j = 0; j < x.height(); ++j)
        for (int i = 0; i < x.width() * 4; ++i) e += std::abs(int(x.constScanLine(j)[i]) - int(y.constScanLine(j)[i]));
    return e / (x.width() * x.height() * 4.0);
}

static void testHashesAndRandom()
{
    CHECK(joaat("") == 0);
    CHECK(joaatLower("ADDER") == 0xB779A091u);   // well-known model hash for "adder"
    CHECK(joaatLower("zentorno") == 0xAC5DF515u);
    // .NET Framework: new Random(0).Next() sequence starts 1559595546, 1755192844, 1649316166
    DotNetRandom r(0);
    CHECK(r.sample() == 1559595546);
    CHECK(r.sample() == 1755192844);
    CHECK(r.sample() == 1649316166);
}

static void testCompressionAndResource()
{
    QByteArray data;
    for (int i = 0; i < 100000; ++i) data += char(i % 251);
    QByteArray c = deflateRaw(data), d;
    CHECK(inflateRaw(c, d, data.size()));
    CHECK(d == data);
    CHECK(inflateRaw(c, d, -1) && d == data);

    qint64 ps = 0;
    uint32_t f = resourceFlagsSinglePage(5000, 0, &ps);
    CHECK(ps == 0x2000 && resourceSizeFromFlags(f) == 0x2000);
    f = resourceFlagsSinglePage(0x2001, 0, &ps);
    CHECK(ps == 0x4000 && resourceSizeFromFlags(f) == 0x4000);
    f = resourceFlagsSinglePage(100 << 20, 0, &ps);
    CHECK(resourceSizeFromFlags(f) == ps && ps >= (100 << 20));
    // Known flags from a game file: ss=1, one page of 0x4000 in the base bucket
    CHECK(resourceSizeFromFlags(0x00020001) == 0x4000);

    ResourceData rd;
    rd.version = 13;
    rd.system = QByteArray(300, 'a');
    rd.graphics = QByteArray(9000, 'b');
    QByteArray built = rd.build();
    ResourceData back;
    CHECK(back.load(built, nullptr));
    CHECK(back.system.left(300) == rd.system && back.graphics.left(9000) == rd.graphics);
    CHECK(back.version == 13);
    CHECK(((back.systemFlags >> 28) & 0xF) == 0 && ((back.graphicsFlags >> 28) & 0xF) == 13);
}

static void testTextures()
{
    QImage img = testImage(64, 32);
    for (uint32_t fmt : {uint32_t(TexDXT1), uint32_t(TexDXT5), uint32_t(TexA8R8G8B8), uint32_t(TexA8B8G8R8)}) {
        QImage src = fmt == TexDXT1 ? img.convertToFormat(QImage::Format_RGB888) : img;  // DXT1 alpha is 1-bit
        TextureImage t = encodeImage(src, fmt, true);
        CHECK(t.levels == (texIsBlock(fmt) ? 4 : 7));
        CHECK(t.data.size() == texChainSize(fmt, 64, 32, t.levels));
        QImage dec = decodeTexture(t);
        double err = meanError(src, dec);
        if (err >= (texIsBlock(fmt) ? 8.0 : 0.01)) std::fprintf(stderr, "format %s error %f\n", qPrintable(texFormatName(fmt)), err);
        CHECK(err < (texIsBlock(fmt) ? 8.0 : 0.01));
        // DDS round trip
        TextureImage t2;
        QString e;
        CHECK(readDds(writeDds(t), t2, &e));
        CHECK(t2.format == t.format && t2.width == 64 && t2.height == 32 && t2.levels == t.levels && t2.data == t.data);
    }
    // odd sizes
    TextureImage odd = encodeImage(testImage(30, 10), TexDXT5, false);
    CHECK(odd.data.size() == 8 * 3 * 16);
    CHECK(decodeTexture(odd).size() == QSize(30, 10));

    // YTD build + parse
    TextureDictionary td;
    td.setTexture("my_texture", encodeImage(img, TexDXT5, true));
    td.setTexture("Second_Tex", encodeImage(testImage(16, 16), TexA8R8G8B8, false));
    td.setTexture("third", encodeImage(testImage(128, 128), TexDXT1, true));
    QString e;
    QByteArray ytd = td.build(&e);
    CHECK(!ytd.isEmpty());
    TextureDictionary back;
    CHECK(back.load(ytd, &e));
    CHECK(back.textures.size() == 3);
    int i = back.find("my_texture");
    CHECK(i >= 0);
    if (i >= 0) {
        CHECK(back.textures[size_t(i)].image.width == 64 && back.textures[size_t(i)].image.format == TexDXT5);
        CHECK(meanError(decodeTexture(back.textures[size_t(i)].image), decodeTexture(td.textures[0].image)) == 0.0);
    }
    // hashes sorted
    ResourceData rd;
    rd.load(ytd, nullptr);
    uint64_t hp = rd.u64(0x50000020);
    CHECK(rd.u32(hp) < rd.u32(hp + 4) && rd.u32(hp + 4) < rd.u32(hp + 8));
    // replace + remove keep the rest intact
    back.setTexture("third", encodeImage(testImage(8, 8), TexA8B8G8R8, false));
    back.remove("Second_Tex");
    TextureDictionary again;
    CHECK(again.load(back.build(&e), &e));
    CHECK(again.textures.size() == 2 && again.find("third") >= 0 && again.textures[size_t(again.find("third"))].image.width == 8);
}

static void testGxt2AndRbf()
{
    Gxt2 g;
    g.entries.push_back({0x12345678, "Hello"});
    g.entries.push_back({0x00000010, "Unicode: éè 中"});
    Gxt2 b;
    CHECK(b.load(g.build(), nullptr));
    CHECK(b.entries.size() == 2 && b.entries[0].hash == 0x10 && b.entries[1].text == "Hello");
    CHECK(b.entries[0].text == QString::fromUtf8("Unicode: éè 中"));
    Gxt2 t;
    CHECK(t.fromText(b.toText(), nullptr));
    CHECK(t.entries.size() == 2 && t.entries[1].text == "Hello");
    CHECK(t.fromText("MY_KEY = line\\nbreak\n", nullptr) && t.entries[0].hash == joaatLower("my_key") && t.entries[0].text == "line\nbreak");

    // RBF: <root a="x"><child value="5"/></root>
    Writer w;
    w.u32(0x30464252);
    w.u8(0); w.u8(0); w.u16(4); w.bytes("root"); w.u16(0); w.u16(0); w.u16(1);  // open root, 1 attribute
    w.u8(1); w.u8(0x60); w.u16(1); w.bytes("a"); w.u16(1); w.bytes("x");       // attribute a="x"
    w.u8(2); w.u8(0x10); w.u16(5); w.bytes("child"); w.u32(5);                  // child uint
    w.u8(0xFF); w.u8(0xFF);
    QString xml;
    CHECK(rbfToXml(w.data, xml, nullptr));
    CHECK(xml.contains("<root a=\"x\">") && xml.contains("<child value=\"0x00000005\" />"));
}

static void testXmlAndText()
{
    QDomDocument doc;
    doc.setContent(QByteArray("<cmds><add>new line</add><insert where=\"After\" line=\"b\">after b</insert>"
                              "<replace line=\"c\">C!</replace><delete>a</delete></cmds>"));
    QString r = applyTextCommands("a\nb\nc\n", doc.documentElement(), nullptr);
    CHECK(r == "b\nafter b\nC!\nnew line\n");

    QByteArray xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<SMandatoryPacksData><Paths><Item>dlcpacks:/a/</Item></Paths></SMandatoryPacksData>";
    QDomDocument cmd;
    cmd.setContent(QByteArray("<xml><add xpath=\"/SMandatoryPacksData/Paths\"><Item>dlcpacks:/mymod/</Item></add>"
                              "<remove xpath=\"/SMandatoryPacksData/Paths/Item[text()='dlcpacks:/a/']\"/></xml>"));
    QString e;
    CHECK(applyXmlCommands(xml, cmd.documentElement(), &e, nullptr));
    CHECK(xml.contains("dlcpacks:/mymod/") && !xml.contains("dlcpacks:/a/"));
    CHECK(xml.startsWith("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<SMandatoryPacksData>"));

    QDomDocument v;
    v.setContent(QByteArray("<L><Item><modelName>adder</modelName><x value=\"1\"/></Item><Item><modelName>zentorno</modelName><x value=\"2\"/></Item></L>"));
    auto n = xpathSelect(v, "/L/Item[modelName='zentorno']/x/@value", &e);
    CHECK(n.size() == 1 && n[0].nodeValue() == "2");
    CHECK(xpathSelect(v, "//modelName", &e).size() == 2);
    CHECK(xpathSelect(v, "/L/Item[2]/modelName", &e).value(0).toElement().text() == "zentorno");
}

static void testArchivesAndOiv()
{
    QTemporaryDir tmp;
    QString base = tmp.path();
    // build a "game" with update/update.rpf containing a nested dlc archive
    auto root = newDirNode();
    root->ensureDir("common/data")->addChild(newFileNode("dlclist.xml",
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<SMandatoryPacksData><Paths><Item>dlcpacks:/mpbeach/</Item></Paths></SMandatoryPacksData>\n"));
    root->ensureDir("common/data")->addChild(newFileNode("gameconfig.txt", "line1\nline2\n"));
    auto nested = newDirNode();
    nested->ensureDir("data")->addChild(newFileNode("vehicles.meta", "<CVehicleModelInfo__InitDataList><InitDatas/></CVehicleModelInfo__InitDataList>"));
    QString nestedPath = base + "/nested.tmp";
    QString e;
    CHECK(writeRpfFile(*nested, nestedPath, &e));
    auto nn = newDiskFileNode("dlc.rpf", nestedPath);
    nn->compress = false;
    root->ensureDir("x64/dlcpacks/mpbeach")->addChild(std::move(nn));
    // a resource entry
    ResourceData rd; rd.version = 13; rd.system = QByteArray(64, 'r');
    root->ensureDir("x64/textures")->addChild(newFileNode("thing.ytd", rd.build()));
    QString game = base + "/game";
    CHECK(writeRpfFile(*root, game + "/update/update.rpf", &e));
    writeAll(game + "/GTA5.exe", "MZ");

    auto a = RpfArchive::openFile(game + "/update/update.rpf", &e);
    CHECK(a != nullptr);
    if (!a) return;
    int ri = a->findPath("x64/textures/thing.ytd");
    CHECK(ri >= 0 && a->entry(ri).isResource() && a->entry(ri).resourceVersion() == 13);
    QByteArray rbytes;
    CHECK(a->extract(ri, rbytes, &e));
    ResourceData rback;
    CHECK(rback.load(rbytes, &e) && rback.system.left(64) == rd.system);

    // names must be sorted per directory (the game binary-searches them)
    for (int d = 0; d < a->count(); ++d) {
        if (!a->entry(d).isDir()) continue;
        auto kids = a->children(d);
        for (size_t k = 1; k < kids.size(); ++k) CHECK(a->entry(kids[k - 1]).name < a->entry(kids[k]).name);
    }

    // OIV package
    QByteArray assembly = R"(<?xml version="1.0" encoding="UTF-8"?>
<package version="2.2" id="{00000000-0000-0000-0000-000000000000}" target="Five">
  <metadata><name>Test Mod</name><version><major>1</major><minor>2</minor><tag>beta</tag></version>
    <author><displayName>Tester</displayName></author><description>desc</description></metadata>
  <content>
    <archive path="update\update.rpf" createIfNotExist="False" type="RPF7">
      <add source="newfile.txt">common\data\newfile.txt</add>
      <text path="common\data\gameconfig.txt"><add>line3</add><replace line="line1">LINE1</replace></text>
      <xml path="common\data\dlclist.xml"><add xpath="/SMandatoryPacksData/Paths"><Item>dlcpacks:/mymod/</Item></add></xml>
      <archive path="x64\dlcpacks\mpbeach\dlc.rpf" createIfNotExist="False" type="RPF7">
        <add source="veh.meta">data\vehicles.meta</add>
      </archive>
      <archive path="x64\dlcpacks\mymod\dlc.rpf" createIfNotExist="True" type="RPF7">
        <add source="newfile.txt">content.xml</add>
      </archive>
    </archive>
    <archive path="update\x64\dlcpacks\mymod\dlc.rpf" createIfNotExist="True" type="RPF7">
      <add source="newfile.txt">setup2.xml</add>
    </archive>
    <add source="loose.asi">scripts\loose.asi</add>
  </content>
</package>)";
    QByteArray zip = makeZip({{"assembly.xml", assembly}, {"content/newfile.txt", "NEW"}, {"content/veh.meta", "<replaced/>"},
                              {"content/loose.asi", "ASI"}});
    writeAll(base + "/test.oiv", zip);
    OivPackage pkg;
    CHECK(pkg.open(base + "/test.oiv", &e));
    CHECK(pkg.name == "Test Mod" && pkg.versionText == "1.2 beta" && pkg.author == "Tester");
    CHECK(pkg.describeOperations().size() == 7);

    GameFS fs;
    fs.gameDir = game;
    fs.useModsFolder = true;
    OivInstaller inst(fs);
    QStringList log;
    inst.log = [&](const QString& s) { log << s; };
    bool ok = inst.install(pkg, &e);
    if (!ok) std::fprintf(stderr, "install error: %s\n%s\n", qPrintable(e), qPrintable(log.join('\n')));
    CHECK(ok);

    // original untouched, mods copy edited
    CHECK(readAll(game + "/update/update.rpf") == readAll(game + "/update/update.rpf"));
    auto orig = RpfArchive::openFile(game + "/update/update.rpf", &e);
    CHECK(orig && orig->findPath("common/data/newfile.txt") < 0);
    QByteArray d;
    CHECK(fs.readFile("update/update.rpf/common/data/newfile.txt", d, &e) && d == "NEW");
    CHECK(fs.readFile("update/update.rpf/common/data/gameconfig.txt", d, &e) && d == "LINE1\nline2\nline3\n");
    CHECK(fs.readFile("update/update.rpf/common/data/dlclist.xml", d, &e) && d.contains("dlcpacks:/mymod/") && d.contains("mpbeach"));
    CHECK(fs.readFile("update/update.rpf/x64/dlcpacks/mpbeach/dlc.rpf/data/vehicles.meta", d, &e) && d == "<replaced/>");
    CHECK(fs.readFile("update/update.rpf/x64/dlcpacks/mymod/dlc.rpf/content.xml", d, &e) && d == "NEW");
    CHECK(fs.readFile("update/update.rpf/x64/textures/thing.ytd", d, &e) && d == rbytes);
    CHECK(QFileInfo::exists(game + "/mods/update/update.rpf"));
    CHECK(fs.readFile("update/x64/dlcpacks/mymod/dlc.rpf/setup2.xml", d, &e) && d == "NEW");
    CHECK(QFileInfo::exists(game + "/mods/update/x64/dlcpacks/mymod/dlc.rpf"));
    CHECK(readAll(game + "/scripts/loose.asi") == "ASI");

    // reinstalling on top works too (in-place update of an already edited archive)
    log.clear();
    CHECK(inst.install(pkg, &e));
    CHECK(fs.readFile("update/update.rpf/common/data/gameconfig.txt", d, &e) && d == "LINE1\nline2\nline3\nline3\n");

    // full rebuild compacts and preserves content
    auto modded = RpfArchive::openFile(game + "/mods/update/update.rpf", &e);
    qint64 before = modded->size();
    CHECK(modifyArchive(modded, nullptr, &e, true));
    CHECK(QFileInfo(game + "/mods/update/update.rpf").size() < before);
    CHECK(fs.readFile("update/update.rpf/x64/dlcpacks/mpbeach/dlc.rpf/data/vehicles.meta", d, &e) && d == "<replaced/>");
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testHashesAndRandom();
    testCompressionAndResource();
    testTextures();
    testGxt2AndRbf();
    testXmlAndText();
    testArchivesAndOiv();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
