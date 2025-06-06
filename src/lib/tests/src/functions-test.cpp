#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QProcess>
#include <QRect>
#include <QSettings>
#include <QTemporaryFile>
#include "functions.h"
#include "models/profile.h"
#include "catch.h"
#include "source-helpers.h"


QDateTime fileCreationDate(const QString &path)
{
	QFileInfo fi(path);
	#ifdef Q_OS_WIN
		#if (QT_VERSION < QT_VERSION_CHECK(5, 10, 0))
			return fi.created();
		#else
			QDateTime d = fi.birthTime();
			if (d.isValid()) {
				return d;
			}
			return fi.metadataChangeTime();
		#endif
	#else
		return fi.lastModified(); // You can't properly change the "creation" time (st_ctime) so we change the mtime on unix
	#endif
}

void assertFixFilename(int platform, const QString &filename, const QString &path, const QString &expected)
{
	QString actual;
	switch (platform)
	{
		case 1: // unix
			actual = fixFilenameLinux(filename, path);
			break;

		default: // windows
			actual = fixFilenameWindows(filename, path);
			break;
	}

	REQUIRE(actual == expected);
}

static QByteArray readFile(const QString &path)
{
	QFile f(path);
	if (!f.open(QFile::ReadOnly)) {
		return QByteArray();
	}

	return f.readAll();
}

static QFont makeFont(const QString &name, int size, bool usePixels, QFont::Weight weight, QFont::Style style)
{
	QFont font(name);
	if (usePixels) {
		font.setPixelSize(size);
	} else {
		font.setPointSize(size);
	}
	font.setWeight(weight);
	font.setStyle(style);
	return font;
}


TEST_CASE("Functions")
{
	SECTION("Fix filename")
	{
		SECTION("Windows")
		{
			assertFixFilename(0, "", "C:\\test\\image.jpg", "C:\\test\\image.jpg");
			assertFixFilename(0, "", "C:\\test", "C:\\test");
			assertFixFilename(0, "", "C:\\test\\", "C:\\test\\");
			assertFixFilename(0, "", " C:\\test\\ ", "C:\\test\\");
			assertFixFilename(0, "image.jpg", "C:\\test\\", "image.jpg");
			assertFixFilename(0, "image", "C:\\test\\", "image");
			assertFixFilename(0, "folder\\image.jpg", "C:\\test\\", "folder\\image.jpg");
			assertFixFilename(0, "folder...\\image.jpg", "C:\\test\\", "folder\\image.jpg");
			assertFixFilename(0, "COM1\\image.jpg", "C:\\test\\", "COM1!\\image.jpg");
			assertFixFilename(0, "com1\\image.jpg", "C:\\test\\", "com1!\\image.jpg");
			assertFixFilename(0, "COM1\\image.jpg", "\\\\?\\C:\\test\\", "COM1\\image.jpg");
		}

		SECTION("Linux")
		{
			assertFixFilename(1, "", "/home/test/image.jpg", "/home/test/image.jpg");
			assertFixFilename(1, "", "/home/test", "/home/test");
			assertFixFilename(1, "", "/home/test/", "/home/test/");
			assertFixFilename(1, "", "/home/test/ ", "/home/test/ ");
			assertFixFilename(1, "image.jpg", "/home/test/", "image.jpg");
			assertFixFilename(1, "image", "/home/test/", "image");
			assertFixFilename(1, "folder/image.jpg", "/home/test/", "folder/image.jpg");

			SECTION("UTF-8")
			{
				const QString utf8Part = "é";
				QString utf8Long;

				// 100 UTF-8 chars / 200 bytes is under the 255 limit
				for (int i = 0; i < 100; ++i) {
					utf8Long += utf8Part;
				}
				REQUIRE(fixFilenameLinux(utf8Long + "/image.jpg", "/home/test/") == QString(utf8Long + "/image.jpg"));
				REQUIRE(fixFilenameLinux(utf8Long + ".jpg", "/home/test/") == QString(utf8Long + ".jpg"));


				// 200 UTF-8 chars / 400 bytes is above the limit so should be cut
				for (int i = 0; i < 100; ++i) {
					utf8Long += utf8Part;
				}
				const QString actual = fixFilenameLinux(utf8Long + "/image.jpg", "/home/test/");
				REQUIRE(actual != QString(utf8Long + "/image.jpg"));
				REQUIRE(actual.length() == 127 + 10);
				REQUIRE(actual.toUtf8().size() == 254 + 10);

				// Same test with the filename instead of dirname
				const QString actualF = fixFilenameLinux(utf8Long + "-image.jpg", "/home/test/");
				REQUIRE(actualF != QString(utf8Long + "-image.jpg"));
				REQUIRE(actualF.length() == 129);
				REQUIRE(actualF.toUtf8().size() == 254);
			}
		}
	}

	SECTION("GetExtensionFromHeader")
	{
		REQUIRE(getExtensionFromHeader(readFile("tests/resources/minimal/gif.gif")) == QString("gif"));
		REQUIRE(getExtensionFromHeader(readFile("tests/resources/minimal/png.png")) == QString("png"));
		REQUIRE(getExtensionFromHeader(readFile("tests/resources/minimal/jpg.jpg")) == QString("jpg"));
		REQUIRE(getExtensionFromHeader(readFile("tests/resources/minimal/bmp.bmp")) == QString("bmp"));
		REQUIRE(getExtensionFromHeader(readFile("tests/resources/minimal/webm.webm")) == QString("webm"));
		REQUIRE(getExtensionFromHeader(readFile("tests/resources/minimal/mp4.mp4")) == QString("mp4"));
		REQUIRE(getExtensionFromHeader(readFile("tests/resources/minimal/swf.swf")) == QString("swf"));
		REQUIRE(getExtensionFromHeader(readFile("tests/resources/minimal/ico.ico")) == QString("ico"));
		REQUIRE(getExtensionFromHeader(readFile("tests/resources/minimal/txt.txt")) == QString());
	}

	SECTION("FontToCss")
	{
		REQUIRE(qFontToCss(makeFont("Arial", 12, false, QFont::Normal, QFont::StyleNormal)) == QString("font-family:'Arial'; font-size:12pt; font-style:normal; font-weight:400; text-decoration:none;"));
		REQUIRE(qFontToCss(makeFont("Arial", 12, true, QFont::Normal, QFont::StyleNormal)) == QString("font-family:'Arial'; font-size:12px; font-style:normal; font-weight:400; text-decoration:none;"));
		REQUIRE(qFontToCss(makeFont("Arial", 12, false, QFont::Bold, QFont::StyleNormal)) == QString("font-family:'Arial'; font-size:12pt; font-style:normal; font-weight:700; text-decoration:none;"));
		REQUIRE(qFontToCss(makeFont("Arial", 12, false, QFont::Normal, QFont::StyleItalic)) == QString("font-family:'Arial'; font-size:12pt; font-style:italic; font-weight:400; text-decoration:none;"));
		REQUIRE(qFontToCss(makeFont("Arial", 12, false, QFont::Normal, QFont::StyleOblique)) == QString("font-family:'Arial'; font-size:12pt; font-style:oblique; font-weight:400; text-decoration:none;"));
	}

	SECTION("IsVariantEmpty")
	{
		// Int
		REQUIRE(isVariantEmpty(QVariant(0)));
		REQUIRE(!isVariantEmpty(QVariant(1)));

		// List
		REQUIRE(isVariantEmpty(QList<QVariant>()));
		REQUIRE(!isVariantEmpty(QList<QVariant>() << 0));
		REQUIRE(!isVariantEmpty(QList<QVariant>() << 1));

		// Map
		REQUIRE(isVariantEmpty(QMap<QString, QVariant>()));
		REQUIRE(!isVariantEmpty(QMap<QString, QVariant> {{ "", 0 }}));
		REQUIRE(!isVariantEmpty(QMap<QString, QVariant> {{ "", 1 }}));

		// String
		REQUIRE(isVariantEmpty(QString()));
		REQUIRE(isVariantEmpty(QString("")));
		REQUIRE(!isVariantEmpty(QString("test")));

		// String list
		REQUIRE(isVariantEmpty(QStringList()));
		REQUIRE(!isVariantEmpty(QStringList() << ""));
		REQUIRE(!isVariantEmpty(QStringList() << "test"));

		// Others
		REQUIRE(!isVariantEmpty(QRect(1, 2, 3, 4)));
	}

	SECTION("GetUnit")
	{
		QStringList units = FILESIZE_UNITS;

		double size1 = 800;
		REQUIRE(getUnit(&size1) == QString(units[0]));
		REQUIRE(size1 == 800.0f);

		double size2 = 2048;
		REQUIRE(getUnit(&size2) == QString(units[1]));
		REQUIRE(size2 == 2.0f);

		double size3 = 7340032;
		REQUIRE(getUnit(&size3) == QString(units[2]));
		REQUIRE(size3 == 7.0f);
	}

	SECTION("FormatFilesize")
	{
		QStringList units = FILESIZE_UNITS;

		REQUIRE(formatFilesize(800) == QString("%1 %2").arg("800", units[0]));
		REQUIRE(formatFilesize(1500) == QString("%1 %2").arg("1.46", units[1]));
		REQUIRE(formatFilesize(2048) == QString("%1 %2").arg("2", units[1]));
		REQUIRE(formatFilesize(5000000) == QString("%1 %2").arg("4.77", units[2]));
		REQUIRE(formatFilesize(7340032) == QString("%1 %2").arg("7", units[2]));
	}

	SECTION("Parse file size")
	{
		REQUIRE(parseFileSize("123") == 123);
		REQUIRE(parseFileSize("12.3") == 12);
		REQUIRE(parseFileSize("123 KB") == (123 * 1024));
		REQUIRE(parseFileSize("1.23 KB") == 1260); // Round(1.23 *  1024)
		REQUIRE(parseFileSize("123 MB") == (123 * 1024 * 1024));
		REQUIRE(parseFileSize("12 GB") == (12. * 1024 * 1024 * 1024));
	}

	SECTION("GetExtension")
	{
		REQUIRE(getExtension(QUrl("")) == QString(""));
		REQUIRE(getExtension(QUrl("https://test.com/file")) == QString(""));
		REQUIRE(getExtension(QUrl("https://test.com/some.dir/file")) == QString(""));
		REQUIRE(getExtension(QUrl("https://test.com/file.jpg")) == QString("jpg"));
		REQUIRE(getExtension(QUrl("https://test.com/file.jpg?toto=1")) == QString("jpg"));
		REQUIRE(getExtension(QUrl("https://test.com/file.jpg:large")) == QString("jpg"));
		REQUIRE(getExtension(QUrl("https://test.com/index.php?image=file.jpg")) == QString("jpg"));
	}
	SECTION("SetExtension")
	{
		REQUIRE(setExtension(QString(""), "png") == QString(""));
		REQUIRE(setExtension(QString("file"), "png") == QString("file"));
		REQUIRE(setExtension(QString("file.jpg"), "png") == QString("file.png"));
		REQUIRE(setExtension(QString("file.jpg"), "") == QString("file"));

		REQUIRE(setExtension(QUrl(""), "png") == QUrl(""));
		REQUIRE(setExtension(QUrl("https://test.com/file"), "png") == QUrl("https://test.com/file"));
		REQUIRE(setExtension(QUrl("https://test.com/file.jpg"), "png") == QUrl("https://test.com/file.png"));
		REQUIRE(setExtension(QUrl("https://test.com/file.jpg?toto=1"), "png") == QUrl("https://test.com/file.png?toto=1"));
		REQUIRE(setExtension(QUrl("https://test.com/file.jpg:large"), "png") == QUrl("https://test.com/file.png:large"));
	}

	SECTION("Levenshtein")
	{
		REQUIRE(levenshtein("", "") == 0);
		REQUIRE(levenshtein("1", "1") == 0);
		REQUIRE(levenshtein("12", "12") == 0);

		REQUIRE(levenshtein("", "1") == 1);
		REQUIRE(levenshtein("", "12") == 2);
		REQUIRE(levenshtein("1", "") == 1);
		REQUIRE(levenshtein("12", "") == 2);

		REQUIRE(levenshtein("password", "password1") == 1);
		REQUIRE(levenshtein("password", "assword") == 1);

		REQUIRE(levenshtein("password", "Xassword") == 1);
		REQUIRE(levenshtein("password", "passXord") == 1);

		REQUIRE(levenshtein("12345678", "23456781") == 2);
		REQUIRE(levenshtein("12345678", "34567812") == 4);
		REQUIRE(levenshtein("12345678", "45678123") == 6);
		REQUIRE(levenshtein("12345678", "56781234") == 8);
		REQUIRE(levenshtein("12345678", "67812345") == 6);
		REQUIRE(levenshtein("12345678", "78123456") == 4);
		REQUIRE(levenshtein("12345678", "81234567") == 2);

		REQUIRE(levenshtein("123", "321") == 2);
		REQUIRE(levenshtein("1234", "4321") == 4);
		REQUIRE(levenshtein("12345", "54321") == 4);
		REQUIRE(levenshtein("123456", "654321") == 6);
		REQUIRE(levenshtein("1234567", "7654321") == 6);
		REQUIRE(levenshtein("12345678", "87654321") == 8);
	}

	SECTION("RemoveWildards")
	{
		REQUIRE(removeWildards(QStringList(), QStringList()) == QStringList());
		REQUIRE(removeWildards(QStringList() << "abc" << "def" << "ghi", QStringList()) == QStringList() << "abc" << "def" << "ghi");
		REQUIRE(removeWildards(QStringList() << "abc" << "def" << "ghi", QStringList() << "a*" << "*f") == QStringList() << "ghi");
		REQUIRE(removeWildards(QStringList() << "abc" << "def" << "ghi", QStringList() << "no_wildcard") == QStringList() << "abc" << "def" << "ghi");
		REQUIRE(removeWildards(QStringList() << "abc" << "def" << "ghi", QStringList() << "*not_found*") == QStringList() << "abc" << "def" << "ghi");
	}

	SECTION("DateTimeFromString")
	{
		// Timestamps
		REQUIRE(qDateTimeFromString("1492192180").toUTC() == QDateTime(QDate(2017, 4, 14), QTime(17, 49, 40), Qt::UTC));

		// Standart dates
		REQUIRE(qDateTimeFromString("2017/04/14 17:49:40.123456").toUTC() == QDateTime(QDate(2017, 4, 14), QTime(17, 49, 40, 123), Qt::UTC));
		REQUIRE(qDateTimeFromString("2017-04-14 17:49:40.123456").toUTC() == QDateTime(QDate(2017, 4, 14), QTime(17, 49, 40, 123), Qt::UTC));
		REQUIRE(qDateTimeFromString("2017/04/14 17:49:40.123").toUTC() == QDateTime(QDate(2017, 4, 14), QTime(17, 49, 40, 123), Qt::UTC));
		REQUIRE(qDateTimeFromString("2017-04-14 17:49:40.123").toUTC() == QDateTime(QDate(2017, 4, 14), QTime(17, 49, 40, 123), Qt::UTC));
		REQUIRE(qDateTimeFromString("2017/04/14 17:49:40").toUTC() == QDateTime(QDate(2017, 4, 14), QTime(17, 49, 40), Qt::UTC));
		REQUIRE(qDateTimeFromString("2017-04-14 17:49:40").toUTC() == QDateTime(QDate(2017, 4, 14), QTime(17, 49, 40), Qt::UTC));
		REQUIRE(qDateTimeFromString("2017/04/14 17:49").toUTC() == QDateTime(QDate(2017, 4, 14), QTime(17, 49), Qt::UTC));
		REQUIRE(qDateTimeFromString("2017-04-14 17:49").toUTC() == QDateTime(QDate(2017, 4, 14), QTime(17, 49), Qt::UTC));

		// Danbooru dates
		REQUIRE(qDateTimeFromString("2017-04-14T17:49:40.498-04:00").toUTC() == QDateTime(QDate(2017, 4, 14), QTime(17 + 4, 49, 40), Qt::UTC));

		// Gelbooru dates
		REQUIRE(qDateTimeFromString("Tue Apr  4 17:49:40 2017").toUTC() == QDateTime(QDate(2017, 4, 4), QTime(17, 49, 40), Qt::UTC));
		REQUIRE(qDateTimeFromString("Fri Apr 14 17:49:40 2017").toUTC() == QDateTime(QDate(2017, 4, 14), QTime(17, 49, 40), Qt::UTC));
		REQUIRE(qDateTimeFromString("Fri Apr 14 17:49:40 -0500 2017").toUTC() == QDateTime(QDate(2017, 4, 14), QTime(17 + 5, 49, 40), Qt::UTC));
		REQUIRE(qDateTimeFromString("Fri Apr 14 23:49:40 -0500 2017").toUTC() == QDateTime(QDate(2017, 4, 15), QTime(4, 49, 40), Qt::UTC));
	}

	SECTION("IsUrl")
	{
		// Valid URLs
		REQUIRE(isUrl("https://foo.com/blah_blah"));
		REQUIRE(isUrl("https://foo.com/blah_blah_(wikipedia)"));
		REQUIRE(isUrl("https://foo.com/blah_(wikipedia)_blah#cite-1"));
		REQUIRE(isUrl("https://foo.com/(something)?after=parens"));
		REQUIRE(isUrl("https://1337.net"));
		REQUIRE(isUrl("https://a.b-c.de"));
		REQUIRE(isUrl("https://223.255.255.254"));

		// Invalid URLs
		REQUIRE(!isUrl("https://"));
		REQUIRE(!isUrl("https://."));
		REQUIRE(!isUrl("https://?"));
		REQUIRE(!isUrl("//"));
		REQUIRE(!isUrl("https:///a"));
		REQUIRE(!isUrl("foo.com"));
	}

	SECTION("ParseMarkdownHeaders")
	{
		REQUIRE(parseMarkdown("# h1") == QString("<h1>h1</h1>"));
		REQUIRE(parseMarkdown("## h2") == QString("<h2>h2</h2>"));
		REQUIRE(parseMarkdown("### h3") == QString("<h3>h3</h3>"));
		REQUIRE(parseMarkdown("#### h4") == QString("<h4>h4</h4>"));
		REQUIRE(parseMarkdown("##### h5") == QString("<h5>h5</h5>"));
		REQUIRE(parseMarkdown("###### h6") == QString("<h6>h6</h6>"));
		REQUIRE(parseMarkdown("####### h7") == QString("<h6>h7</h6>"));
		REQUIRE(parseMarkdown("a # h1") == QString("a # h1"));
	}
	SECTION("ParseMarkdownIssueLinks")
	{
		REQUIRE(parseMarkdown("issue #123") == QString("issue <a href='" + QString(PROJECT_GITHUB_URL) + "/issues/123'>#123</a>"));
		REQUIRE(parseMarkdown("fix #123") == QString("fix <a href='" + QString(PROJECT_GITHUB_URL) + "/issues/123'>#123</a>"));
		REQUIRE(parseMarkdown("issue 123") == QString("issue 123"));
	}

	SECTION("SetFileCreationDate")
	{
		QString path = "tests/resources/pages/behoimi.org/results.json";
		QDateTime date = QDateTime::currentDateTimeUtc();

		REQUIRE(setFileCreationDate(path, date));

		QDateTime created = fileCreationDate(path);
		REQUIRE(created.toSecsSinceEpoch() == date.toSecsSinceEpoch());
	}
	SECTION("SetFileCreationDateUtf8")
	{
		QString path = "tests/resources/你好.txt";
		QDateTime date = QDateTime::currentDateTimeUtc();

		REQUIRE(setFileCreationDate(path, date));

		QDateTime created = fileCreationDate(path);
		REQUIRE(created.toSecsSinceEpoch() == date.toSecsSinceEpoch());
	}

	SECTION("GetExternalLogFilesSuffixes")
	{
		auto *profile = makeProfile();
		auto *settings = profile->getSettings();

		REQUIRE(getExternalLogFilesSuffixes(settings) == QStringList());

		settings->setValue("LogFiles/0/locationType", 1);
		settings->setValue("LogFiles/0/uniquePath", "path");
		settings->setValue("LogFiles/0/content", "id: %id%");

		REQUIRE(getExternalLogFilesSuffixes(settings) == QStringList());

		settings->setValue("LogFiles/0/locationType", 2);
		settings->setValue("LogFiles/0/suffix", ".xml");

		REQUIRE(getExternalLogFilesSuffixes(settings) == QStringList() << ".xml");

		settings->remove("LogFiles/0/locationType");
		settings->remove("LogFiles/0/suffix");
		settings->remove("LogFiles/0/uniquePath");
		settings->remove("LogFiles/0/content");

		delete profile;
	}

	SECTION("FixCloudflareEmail")
	{
		REQUIRE(fixCloudflareEmail("145d505b58595447405146") == QString("IDOLM@STER"));
		REQUIRE(fixCloudflareEmail("cc9cbea3a6a9afb8e1a5818c9f") == QString("Project-iM@S"));
	}
	SECTION("FixCloudflareEmails")
	{
		REQUIRE(fixCloudflareEmails(R"(<a class="dtext-link dtext-wiki-link" href="/wiki_pages/show_or_new?title=idolm%40ster_cinderella_girls"><span class="__cf_email__" data-cfemail="145d505b58595447405146">[email&#160;protected]</span> Cinderella Girls</a>)") == QString(R"(<a class="dtext-link dtext-wiki-link" href="/wiki_pages/show_or_new?title=idolm%40ster_cinderella_girls">IDOLM@STER Cinderella Girls</a>)"));
		REQUIRE(fixCloudflareEmails(R"(Koshimizu Sachiko on <span class="__cf_email__" data-cfemail="cc9cbea3a6a9afb8e1a5818c9f">[email&#160;protected]</span>)") == QString("Koshimizu Sachiko on Project-iM@S"));
	}

	SECTION("GetFileMd5")
	{
		REQUIRE(getFileMd5(QString()) == QString());
		REQUIRE(getFileMd5("non_existing_path.txt") == QString());

		QTemporaryFile file;
		REQUIRE(file.open());
		file.write("test");
		file.seek(0);

		REQUIRE(getFileMd5(file.fileName()) == QString("098f6bcd4621d373cade4e832627b4f6")); // md5("test")
	}
	SECTION("GetFilenameMd5")
	{
		REQUIRE(getFilenameMd5("", "%md5%.%ext%") == QString());
		REQUIRE(getFilenameMd5("lol.jpg", "%md5%.%ext%") == QString());
		REQUIRE(getFilenameMd5("test/098f6bcd4621d373cade4e832627b4f6.jpg", "%md5%.%ext%") == QString());
		REQUIRE(getFilenameMd5("123456789", "%md5%") == QString());

		REQUIRE(getFilenameMd5("098f6bcd4621d373cade4e832627b4f6", "%md5%") == QString("098f6bcd4621d373cade4e832627b4f6"));
		REQUIRE(getFilenameMd5("098f6bcd4621d373cade4e832627b4f6.jpg", "%md5%.%ext%") == QString("098f6bcd4621d373cade4e832627b4f6"));
		REQUIRE(getFilenameMd5("test/098f6bcd4621d373cade4e832627b4f6.jpg", "%artist%/%md5%.%ext%") == QString("098f6bcd4621d373cade4e832627b4f6"));

		#ifdef Q_OS_WIN
			REQUIRE(getFilenameMd5("test/098f6bcd4621d373cade4e832627b4f6.jpg", "%artist%\\%md5%.%ext%") == QString("098f6bcd4621d373cade4e832627b4f6"));
		#endif
	}
	SECTION("GetFilenameId")
	{
		REQUIRE(getFilenameId("", "%id%.%ext%") == QString());
		REQUIRE(getFilenameId("lol.jpg", "%id%.%ext%") == QString());
		REQUIRE(getFilenameId("test/123456789.jpg", "%id%.%ext%") == QString());
		REQUIRE(getFilenameId("098f6bcd4621d373cade4e832627b4f6", "%id%") == QString());

		REQUIRE(getFilenameId("123456789", "%id%") == QString("123456789"));
		REQUIRE(getFilenameId("123456789.jpg", "%id%.%ext%") == QString("123456789"));
		REQUIRE(getFilenameId("test/123456789.jpg", "%artist%/%id%.%ext%") == QString("123456789"));

		#ifdef Q_OS_WIN
			REQUIRE(getFilenameId("test/123456789.jpg", "%artist%\\%id%.%ext%") == QString("123456789"));
		#endif
	}

	SECTION("RemoveCacheBuster")
	{
		REQUIRE(removeCacheBuster(QUrl("https://test.com")) == QUrl("https://test.com"));
		REQUIRE(removeCacheBuster(QUrl("https://test.com?string")) == QUrl("https://test.com?string"));
		REQUIRE(removeCacheBuster(QUrl("https://test.com?1234")) == QUrl("https://test.com"));
		REQUIRE(removeCacheBuster(QUrl("https://test.com/path")) == QUrl("https://test.com/path"));
		REQUIRE(removeCacheBuster(QUrl("https://test.com/path?string")) == QUrl("https://test.com/path?string"));
		REQUIRE(removeCacheBuster(QUrl("https://test.com/path?1234")) == QUrl("https://test.com/path"));
	}

	SECTION("splitStringMulti")
	{
		SECTION("Basic usage")
		{
			REQUIRE(splitStringMulti({}, "a,b;c,d") == QStringList { "a,b;c,d" });
			REQUIRE(splitStringMulti({ ';' }, "a,b;c,d") == QStringList { "a,b", "c,d" });
			REQUIRE(splitStringMulti({ ',' }, "a,b;c,d") == QStringList { "a", "b;c", "d" });
			REQUIRE(splitStringMulti({ ',', ';' }, "a,b;c,d") == QStringList { "a", "b", "c", "d" });
		}

		SECTION("Skip empty parts")
		{
			REQUIRE(splitStringMulti({ ',', ';' }, ",;,a,b;c,d", false) == QStringList { "", "", "", "a", "b", "c", "d" });
			REQUIRE(splitStringMulti({ ',', ';' }, ",;,a,b;c,d", true) == QStringList { "a", "b", "c", "d" });
			REQUIRE(splitStringMulti({ ',', ';' }, "a,,b;;c,d", false) == QStringList { "a", "", "b", "", "c", "d" });
			REQUIRE(splitStringMulti({ ',', ';' }, "a,,b;;c,d", true) == QStringList { "a", "b", "c", "d" });
			REQUIRE(splitStringMulti({ ',', ';' }, "a,b;c,d,;,", false) == QStringList { "a", "b", "c", "d", "", "", "" });
			REQUIRE(splitStringMulti({ ',', ';' }, "a,b;c,d,;,", true) == QStringList { "a", "b", "c", "d" });
		}
	}

	SECTION("decodeHtmlEntities")
	{
		REQUIRE(decodeHtmlEntities("pok&eacute;mon") == QString("pokémon"));
		REQUIRE(decodeHtmlEntities("a&amp;b") == QString("a&b"));
	}

	SECTION("splitCommand")
	{
		SECTION("Basic usage")
		{
			REQUIRE(splitCommand("") == QStringList {});
			REQUIRE(splitCommand("a") == QStringList { "a" });
			REQUIRE(splitCommand("a b c") == QStringList { "a", "b", "c" });
		}

		SECTION("Multiple spaces")
		{
			REQUIRE(splitCommand(" ") == QStringList {});
			REQUIRE(splitCommand(" a ") == QStringList { "a" });
			REQUIRE(splitCommand(" a  b   c ") == QStringList { "a", "b", "c" });
		}

		SECTION("Backslash escape")
		{
			REQUIRE(splitCommand("a\\ b c") == QStringList { "a b", "c" });
			REQUIRE(splitCommand("a\\\\ b c") == QStringList { "a\\", "b", "c" });
			REQUIRE(splitCommand("\\\"a b\\\" c") == QStringList { "\"a", "b\"", "c" });
		}

		SECTION("Double quote escape")
		{
			REQUIRE(splitCommand("\"a b\" c") == QStringList { "a b", "c" });
			REQUIRE(splitCommand("a \"b c\"") == QStringList { "a", "b c" });
			REQUIRE(splitCommand("\"a b c\"") == QStringList { "a b c" });
			REQUIRE(splitCommand("\"a b \"\"\" c\"") == QStringList { "a b \" c" });
		}

		SECTION("Single quote escape")
		{
			REQUIRE(splitCommand("'a b' c") == QStringList { "a b", "c" });
			REQUIRE(splitCommand("a 'b c'") == QStringList { "a", "b c" });
			REQUIRE(splitCommand("'a b c'") == QStringList { "a b c" });
			REQUIRE(splitCommand("'a b ''' c'") == QStringList { "a b ' c" });
		}

		SECTION("Mixed quotes escape")
		{
			REQUIRE(splitCommand("'a' b \"c\"") == QStringList { "a", "b", "c" });
			REQUIRE(splitCommand("'a \"b\"' c") == QStringList { "a \"b\"", "c" });
			REQUIRE(splitCommand("a '\"b\" c'") == QStringList { "a", "\"b\" c" });
			REQUIRE(splitCommand("\"a 'b'\" c") == QStringList { "a 'b'", "c" });
			REQUIRE(splitCommand("a \"'b' c\"") == QStringList { "a", "'b' c" });
			REQUIRE(splitCommand("a \"'b' \"\"\" c\"") == QStringList { "a", "'b' \" c" });
		}

		SECTION("Consistent with QProcess::splitCommand")
		{
			static const QStringList tests {
				"",
				" ",
				"a",
				" a ",
				"a b c",
				" a  b   c ",
				"\"a b\" c",
				"a \"b c\"",
				"\"a b c\"",
				"\"a b \"\"\" c\"",
			};

			for (const QString &str : tests) {
				REQUIRE(splitCommand(str) == QProcess::splitCommand(str));
			}
		}
	}

	SECTION("getKeySequence")
	{
		auto *profile = makeProfile();
		auto *settings = profile->getSettings();

		settings->setValue("exists", QKeySequence("E"));

		REQUIRE(getKeySequence(settings, "exists", Qt::Key_D).toString() == QString("E"));
		REQUIRE(getKeySequence(settings, "not-found", Qt::Key_D).toString() == QString("D"));
		REQUIRE(getKeySequence(settings, "not-found", QKeySequence::Open, Qt::Key_D).toString() == QString("Ctrl+O"));

		#ifndef Q_OS_MAC
			// On macOS, QKeySequence::Preferences is defined, so it would return "Ctrl+," rather than "D"
			REQUIRE(getKeySequence(settings, "not-found", QKeySequence::Preferences, Qt::Key_D).toString() == QString("D"));
		#endif
	}

	SECTION("rectToString")
	{
		REQUIRE(rectToString(QRect()) == QString());
		REQUIRE(rectToString(QRect(1, 2, 3, 4)) == QString("1;2;3;4"));
	}
	SECTION("stringToRect")
	{
		REQUIRE(stringToRect("") == QRect());
		REQUIRE(stringToRect("1;2;3;4") == QRect(1, 2, 3, 4));
	}

	SECTION("renameSettingsKey")
	{
		QSettings settings("tests/resources/tmp/settings.ini", QSettings::IniFormat);
		settings.setValue("before", 123);

		renameSettingsKey(&settings, "before", "after");

		REQUIRE(settings.value("before", -1).toInt() == -1);
		REQUIRE(settings.value("after", -1).toInt() == 123);
	}
	SECTION("renameSettingsGroup")
	{
		QSettings settings("tests/resources/tmp/settings.ini", QSettings::IniFormat);
		settings.setValue("before/key", 123);
		settings.setValue("before/group/key", 123);

		renameSettingsGroup(&settings, "before", "after");

		REQUIRE(settings.value("before/group/key", -1).toInt() == -1);
		REQUIRE(settings.value("after/group/key", -1).toInt() == 123);
	}
}
