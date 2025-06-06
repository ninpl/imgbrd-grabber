#include "language-loader.h"
#include <QCoreApplication>
#include <QDir>
#include <QLocale>
#include <QMap>
#include <QSettings>
#include <utility>
#include "logger.h"


LanguageLoader::LanguageLoader(QString path)
	: m_path(std::move(path))
{}

QMap<QString, QString> LanguageLoader::getAllLanguages() const
{
	QSettings fullLanguages(m_path + "languages.ini", QSettings::IniFormat);

	QStringList languageFiles = QDir(m_path).entryList({ QStringLiteral("*.qm") }, QDir::Files);
	QMap<QString, QString> languages;
	for (const QString &languageFile : languageFiles) {
		const QString lang = languageFile.left(languageFile.length() - 3);
		const QString fullLang = fullLanguages.value(lang, lang).toString();
		languages[lang] = fullLang;
	}

	if (!languages.contains("English")) {
		languages[""] = "English";
	}

	return languages;
}

bool LanguageLoader::install(QCoreApplication *app)
{
	const bool general = app->installTranslator(&m_translator);
	const bool qt = app->installTranslator(&m_qtTranslator);
	return general && qt;
}

bool LanguageLoader::uninstall(QCoreApplication *app)
{
	const bool general = app->removeTranslator(&m_translator);
	const bool qt = app->removeTranslator(&m_qtTranslator);
	return general && qt;
}

bool LanguageLoader::setLanguage(const QString &lang, bool useSystemLocale)
{
	log(QStringLiteral("Setting language to '%1' (`%2`)...").arg(lang, m_path + lang + ".qm"), Logger::Info);

	const bool general = m_translator.load(m_path + lang + ".qm");
	const bool qt = m_qtTranslator.load(m_path + "qt/" + lang + ".qm");

	QLocale::setDefault(useSystemLocale ? QLocale::system() : localeFromString(lang));

	emit languageChanged();

	return general && qt;
}

QLocale LanguageLoader::localeFromString(const QString &lang)
{
	// Special case for Chinese because we need to set the "script" parameter
	if (lang == QStringLiteral("ChineseSimplified")) {
		return QLocale(QLocale::Chinese, QLocale::SimplifiedChineseScript, QLocale::AnyCountry);
	}
	if (lang == QStringLiteral("ChineseTraditional")) {
		return QLocale(QLocale::Chinese, QLocale::TraditionalChineseScript, QLocale::AnyCountry);
	}

	// Special case for country language variants
	if (lang == QStringLiteral("PortugueseBrazilian")) {
		return QLocale(QLocale::Portuguese, QLocale::Brazil);
	}

	static const QMap<QString, QLocale::Language> languages
	{
		{ "Arabic", QLocale::Arabic },
		{ "Dutch", QLocale::Dutch },
		{ "English", QLocale::English },
		{ "French", QLocale::French },
		{ "German", QLocale::German },
		{ "Indonesian", QLocale::Indonesian },
		{ "Italian", QLocale::Italian },
		{ "Japanese", QLocale::Japanese },
		{ "Korean", QLocale::Korean },
		{ "Polish", QLocale::Polish },
		{ "Portuguese", QLocale::Portuguese },
		{ "Russian", QLocale::Russian },
		{ "Spanish", QLocale::Spanish },
		{ "Turkish", QLocale::Turkish },
		{ "Ukrainian", QLocale::Ukrainian },
		{ "Vietnamese", QLocale::Vietnamese },
	};
	if (languages.contains(lang)) {
		// If using the same language as the system, directly use the system's locale
		if (QLocale::system().language() == languages[lang]) {
			return QLocale::system();
		}

		return QLocale(languages[lang]);
	}

	return QLocale(tr("en_US")); // TODO: only use the line below after translations are updated
}
