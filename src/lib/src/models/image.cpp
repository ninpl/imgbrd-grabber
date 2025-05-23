#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>
#include <utility>
#include "commands/commands.h"
#include "downloader/extension-rotator.h"
#include "external/exiftool.h"
#include "external/ffmpeg.h"
#include "external/image-magick.h"
#include "favorite.h"
#include "filename/filename.h"
#include "filtering/tag-filter-list.h"
#include "functions.h"
#include "loader/token.h"
#include "logger.h"
#include "models/api/api.h"
#include "models/api/api-endpoint.h"
#include "models/image.h"
#include "models/page.h"
#include "models/pool.h"
#include "models/profile.h"
#include "models/site.h"
#include "network/network-reply.h"
#include "tags/tag.h"
#include "tags/tag-database.h"
#include "tags/tag-stylist.h"
#include "tags/tag-type.h"
#include "utils/size-utils.h"
#ifdef WIN_FILE_PROPS
	#include "windows-file-property.h"
#endif

#define MAX_LOAD_FILESIZE (1024 * 1024 * 50)


Image::Image()
	: m_profile(nullptr), m_extensionRotator(nullptr)
{}

// TODO(Bionus): clean up this mess
Image::Image(const Image &other)
	: QObject(other.parent())
{
	m_parent = other.m_parent;
	m_parentUrl = other.m_parentUrl;
	m_isGallery = other.m_isGallery;

	m_id = other.m_id;

	m_url = other.m_url;
	m_md5 = other.m_md5;
	m_name = other.m_name;
	m_sources = other.m_sources;

	m_pageUrl = other.m_pageUrl;

	m_sizes = other.m_sizes;
	m_identity = other.m_identity;
	m_data = other.m_data;

	m_galleryCount = other.m_galleryCount;
	m_position = other.m_position;

	m_loadDetails = other.m_loadDetails;

	m_tags = other.m_tags;
	m_pools = other.m_pools;
	m_profile = other.m_profile;
	m_settings = other.m_settings;
	m_search = other.m_search;
	m_parentSite = other.m_parentSite;

	m_extensionRotator = other.m_extensionRotator;
	m_loadingDetails = other.m_loadingDetails;
}

Image::Image(Profile *profile)
	: m_profile(profile), m_settings(profile->getSettings())
{}

Image::Image(Site *site, QMap<QString, QString> details, Profile *profile, Page *parent)
	: Image(site, std::move(details), QVariantMap(), QVariantMap(), profile, parent)
{}

Image::Image(Site *site, QMap<QString, QString> details, QVariantMap identity, QVariantMap data, Profile *profile, Page *parent)
	: m_profile(profile), m_parent(parent), m_id(0), m_parentSite(site), m_extensionRotator(nullptr), m_identity(std::move(identity)), m_data(std::move(data))
{
	m_settings = m_profile->getSettings();

	// Parents
	if (m_parentSite == nullptr) {
		log(QStringLiteral("Image has nullptr parent, aborting creation."));
		return;
	}
	if (m_parent != nullptr) {
		m_parentUrl = m_parent->url();
	}

	// Other details
	m_isGallery = details.contains("type") && details["type"] == "gallery";
	m_md5 = details.contains("md5") ? details["md5"] : "";
	m_name = details.contains("name") ? details["name"] : "";
	m_search = parent != nullptr ? parent->search() : (details.contains("search") ? details["search"].split(' ') : QStringList());
	m_id = details.contains("id") ? details["id"].toULongLong() : 0;
	m_sources = details.contains("sources") ? details["sources"].split('\n') : (details.contains("source") ? QStringList { details["source"] } : QStringList());
	m_galleryCount = details.contains("gallery_count") ? details["gallery_count"].toInt() : -1;
	m_position = details.contains("position") ? details["position"].toInt() : 0;

	// Sizes
	static const QMap<Image::Size, QString> prefixes
	{
		{ Image::Size::Full, "" },
		{ Image::Size::Sample, "sample_" },
		{ Image::Size::Thumbnail, "preview_" },
	};
	for (auto it = prefixes.constBegin(); it != prefixes.constEnd(); ++it) {
		const QString &prefix = it.value();

		auto is = QSharedPointer<ImageSize>::create();

		const QString &urlKey = (prefix.isEmpty() ? "file_" : prefix) + "url";
		is->url = details.contains(urlKey) ? removeCacheBuster(m_parentSite->fixUrl(details[urlKey])) : QString();

		const int width = details.value(prefix + "width", "0").toInt();
		const int height = details.value(prefix + "height", "0").toInt();
		is->size = width > 0 && height > 0 ? QSize(width, height) : QSize();
		is->fileSize = details.contains(prefix + "file_size") ? details[prefix + "file_size"].toInt() : 0;

		if (details.contains(prefix + "rect")) {
			const QStringList rect = details[prefix + "rect"].split(';');
			if (rect.count() == 4) {
				is->rect = QRect(rect[0].toInt(), rect[1].toInt(), rect[2].toInt(), rect[3].toInt());
			} else {
				log("Invalid number of values for image rectangle", Logger::Error);
			}
		}

		m_sizes.insert(it.key(), is);
		m_allSizes.append(is);
	}

	// Medias
	if (m_data.contains("medias")) {
		const auto medias = m_data["medias"].value<QList<QSharedPointer<ImageSize>>>();
		m_data.remove("medias");

		QSharedPointer<ImageSize> preview = m_sizes.value(Image::Thumbnail, nullptr);
		QSharedPointer<ImageSize> sample = m_sizes.value(Image::Sample, nullptr);
		QSharedPointer<ImageSize> full = m_sizes.value(Image::Full, nullptr);
		QMap<Image::Size, QSize> sizes = {
			{ Image::Thumbnail, preview ? preview->size : QSize() },
			{ Image::Sample, sample ? sample->size : QSize() },
			{ Image::Full, full ? full->size : QSize() },
		};

		for (const auto &media : medias) {
			const Image::Size type = media->type;
			const QSize size = media->size;

			m_allSizes.append(media);

			// If type is provided, trust it
			if (type != Image::Unknown) {
				m_sizes.insert(type, media);
				sizes.insert(type, size);
				continue;
			}

			// Preview gets the biggest size between 150 and 300
			if (
				sizes[Image::Thumbnail].isEmpty() || // Default
				(isInRange(size, 150, 300) && (
					 isBigger(size, sizes[Image::Thumbnail]) || // Biggest under 300px
					 !isInRange(sizes[Image::Thumbnail], 150, 300)) // If the default was bigger than 300px
				)
			) {
				m_sizes.insert(Image::Thumbnail, media);
				sizes[Image::Thumbnail] = size;
			}

			// Sample is optional and takes the biggest size between 500 and 1500
			if (isInRange(size, 500, 1500) && isBigger(size, sizes[Image::Sample])) {
				m_sizes.insert(Image::Sample, media);
				sizes[Image::Sample] = size;
			}

			// Full just takes the biggest size available
			if (isBigger(size, sizes[Image::Full])) {
				m_sizes.insert(Image::Full, media);
				sizes[Image::Full] = size;
			}
		}
	}

	// Page url
	if (details.contains("page_url")) {
		m_pageUrl = m_parentSite->fixUrl(m_parentSite->fixLoginUrl(details["page_url"]));
	}

	// Tags
	if (m_data.contains("tags")) {
		m_tags = m_data["tags"].value<QList<Tag>>();
		m_data.remove("tags");
	}

	// Complete missing tag type information
	m_parentSite->tagDatabase()->load();
	QStringList unknownTags;
	for (const Tag &tag : qAsConst(m_tags)) {
		if (tag.type().isUnknown()) {
			unknownTags.append(tag.text());
		}
	}
	QMap<QString, TagType> dbTypes = m_parentSite->tagDatabase()->getTagTypes(unknownTags);
	for (Tag &tag : m_tags) {
		if (dbTypes.contains(tag.text())) {
			tag.setType(dbTypes[tag.text()]);
		}
	}

	// Get file url and try to improve it to save bandwidth
	m_url = m_sizes[Size::Full]->url;
	const QString ext = getExtension(m_url);
	if (details.contains("ext") && !details["ext"].isEmpty()) {
		const QString realExt = details["ext"];
		if (ext != realExt) {
			setFileExtension(realExt);
			m_extension = realExt;
		}
	} else if (ext == QLatin1String("jpg") && !url(Size::Thumbnail).isEmpty()) {
		bool fixed = false;
		const QString previewExt = getExtension(url(Size::Thumbnail));
		if (!url(Size::Sample).isEmpty()) {
			// Guess extension from sample url
			const QString sampleExt = getExtension(url(Size::Sample));
			if (sampleExt != QLatin1String("jpg") && sampleExt != QLatin1String("png") && sampleExt != ext && previewExt == ext) {
				m_url = setExtension(m_url, sampleExt);
				fixed = true;
			}
		}

		// Guess the extension from the tags
		if (!fixed) {
			if ((hasTag(QStringLiteral("swf")) || hasTag(QStringLiteral("flash"))) && ext != QLatin1String("swf")) {
				setFileExtension(QStringLiteral("swf"));
			} else if ((hasTag(QStringLiteral("gif")) || hasTag(QStringLiteral("animated_gif"))) && ext != QLatin1String("webm") && ext != QLatin1String("mp4")) {
				setFileExtension(QStringLiteral("gif"));
			} else if (hasTag(QStringLiteral("mp4")) && ext != QLatin1String("gif") && ext != QLatin1String("webm")) {
				setFileExtension(QStringLiteral("mp4"));
			} else if (hasTag(QStringLiteral("animated_png")) && ext != QLatin1String("webm") && ext != QLatin1String("mp4")) {
				setFileExtension(QStringLiteral("png"));
			} else if ((hasTag(QStringLiteral("webm")) || hasTag(QStringLiteral("animated"))) && ext != QLatin1String("gif") && ext != QLatin1String("mp4")) {
				setFileExtension(QStringLiteral("webm"));
			}
		}
	} else if (details.contains("image") && details["image"].contains("MB // gif\" height=\"") && ext != QLatin1String("gif")) {
		m_url = setExtension(m_url, QStringLiteral("gif"));
	} else if (ext == QLatin1String("webm") && hasTag(QStringLiteral("mp4"))) {
		m_url = setExtension(m_url, QStringLiteral("mp4"));
	}

	// Remove ? in urls
	m_url = removeCacheBuster(m_url);

	init();
}

void Image::init()
{
	// Page URL
	if (m_pageUrl.isEmpty()) {
		Api *api = m_parentSite->detailsApi();
		if (api != nullptr) {
			m_pageUrl = api->detailsUrl(m_id, m_md5, m_parentSite, m_identity).url;
		}
	}
	m_pageUrl = m_parentSite->fixUrl(m_pageUrl).toString();

	// Setup extension rotator
	const bool animated = hasTag("gif") || hasTag("animated_gif") || hasTag("mp4") || hasTag("animated_png") || hasTag("webm") || hasTag("animated") || hasTag("video");
	const QStringList extensions = animated
		? QStringList { "mp4", "webm", "gif", "jpg", "png", "jpeg", "swf" }
		: QStringList { "jpg", "png", "gif", "jpeg", "webm", "swf", "mp4" };
	m_extensionRotator = new ExtensionRotator(getExtension(m_url), extensions, this);
}


static const QMap<Image::Size, QString> sizeToStringMap
{
	{ Image::Size::Full, "full" },
	{ Image::Size::Sample, "sample" },
	{ Image::Size::Thumbnail, "thumbnail" },
};

void Image::write(QJsonObject &json) const
{
	json["website"] = m_parentSite->url();

	// Parent gallery
	if (!m_parentGallery.isNull()) {
		QJsonObject jsonGallery;
		m_parentGallery->write(jsonGallery);
		json["gallery"] = jsonGallery;
	}

	// Sizes
	QJsonObject jsonSizes;
	for (const auto &size : m_sizes.keys()) {
		QJsonObject jsonSize;
		m_sizes[size]->write(jsonSize);
		if (!jsonSize.isEmpty() && sizeToStringMap.contains(size)) {
			jsonSizes[sizeToStringMap[size]] = jsonSize;
		}
	}
	if (!jsonSizes.isEmpty()) {
		json["sizes"] = jsonSizes;
	}

	// Tags
	QJsonArray tags;
	for (const Tag &tag : m_tags) {
		QJsonObject jsonTag;
		tag.write(jsonTag);
		tags.append(jsonTag);
	}

	// FIXME: real serialization
	json["name"] = m_name;
	json["id"] = QString::number(m_id);
	json["md5"] = m_md5;
	json["tags"] = tags;
	json["url"] = m_url.toString();
	json["search"] = QJsonArray::fromStringList(m_search);

	// Arbitrary tokens
	QJsonObject jsonData;
	for (const auto &key : m_data.keys()) {
		jsonData[key] = (QMetaType::Type) m_data[key].type() == QMetaType::QDateTime
			? "date:" + m_data[key].toDateTime().toString(Qt::ISODate)
			: QJsonValue::fromVariant(m_data[key]);
	}
	if (!jsonData.isEmpty()) {
		json["data"] = jsonData;
	}

	// Identity
	QJsonObject jsonIdentity;
	for (const auto &key : m_identity.keys()) {
		jsonIdentity[key] = QJsonValue::fromVariant(m_identity[key]);
	}
	if (!jsonIdentity.isEmpty()) {
		json["identity"] = jsonIdentity;
	}
}

bool Image::read(const QJsonObject &json, const QMap<QString, Site*> &sites)
{
	const QString site = json["website"].toString();
	if (!sites.contains(site)) {
		log(QStringLiteral("Unknown site: %1").arg(site), Logger::Warning);
		return false;
	}

	if (json.contains("gallery")) {
		auto *gallery = new Image(m_profile);
		if (gallery->read(json["gallery"].toObject(), sites)) {
			m_parentGallery = QSharedPointer<Image>(gallery);
		} else {
			gallery->deleteLater();
			return false;
		}
	}

	m_parentSite = sites[site];

	// Sizes
	for (const auto &size : sizeToStringMap.keys()) {
		auto sizeObj = QSharedPointer<ImageSize>::create();
		const QString &key = sizeToStringMap[size];
		if (json.contains("sizes")) {
			const auto &jsonSizes = json["sizes"].toObject();
			if (jsonSizes.contains(key)) {
				sizeObj->read(jsonSizes[key].toObject());
			}
		}
		m_sizes[size] = sizeObj;
	}

	// Tags
	QJsonArray jsonTags = json["tags"].toArray();
	m_tags.reserve(jsonTags.count());
	for (const auto &jsonTag : jsonTags) {
		if (jsonTag.isString()) {
			m_tags.append(Tag(jsonTag.toString()));
		} else {
			Tag tag;
			if (tag.read(jsonTag.toObject())) {
				m_tags.append(tag);
			}
		}
	}

	// Search
	QJsonArray jsonSearch = json["search"].toArray();
	m_search.reserve(jsonSearch.count());
	for (const auto &tag : jsonSearch) {
		m_search.append(tag.toString());
	}

	// Basic fields
	m_name = json["name"].toString();
	m_id = json["id"].toString().toULongLong();
	m_md5 = json["md5"].toString();

	// Arbitrary tokens
	if (json.contains("data")) {
		const auto &jsonData = json["data"].toObject();
		for (const auto &key : jsonData.keys()) {
			QVariant val = jsonData[key].toVariant();
			if (val.toString().startsWith("date:")) {
				val = QDateTime::fromString(val.toString().mid(5), Qt::ISODate);
			}
			m_data[key] = val;
		}
	}

	// Identity
	if (json.contains("identity")) {
		const auto &jsonIdentity = json["identity"].toObject();
		for (const auto &key : jsonIdentity.keys()) {
			m_identity[key] = jsonIdentity[key].toVariant();
		}
	}

	// URL with fallback
	if (json.contains("file_url")) {
		m_url = json["file_url"].toString();
		if (m_sizes[Size::Full]->url.isEmpty()) {
			m_sizes[Size::Full]->url = m_url;
		}
	} else {
		m_url = json.contains("url") ? json["url"].toString() : m_sizes[Size::Full]->url;
	}

	init();
	return true;
}


void Image::loadDetails(bool rateLimit)
{
	if (m_loadingDetails) {
		return;
	}

	if (m_loadedDetails || m_pageUrl.isEmpty()) {
		emit finishedLoadingTags(LoadTagsResult::Ok);
		return;
	}

	if (m_loadDetails != nullptr) {
		if (m_loadDetails->isRunning()) {
			m_loadDetails->abort();
		}

		m_loadDetails->deleteLater();
	}

	log(QStringLiteral("Loading image details from `%1`").arg(m_pageUrl.toString()), Logger::Info);

	Site::QueryType type = rateLimit ? Site::QueryType::Retry : Site::QueryType::Details;
	m_loadDetails = m_parentSite->get(m_pageUrl, type);
	m_loadDetails->setParent(this);
	m_loadingDetails = true;

	connect(m_loadDetails, &NetworkReply::finished, this, &Image::parseDetails);
}
void Image::abortTags()
{
	if (m_loadingDetails && m_loadDetails->isRunning()) {
		m_loadDetails->abort();
		m_loadingDetails = false;
	}
}
void Image::parseDetails()
{
	m_loadingDetails = false;

	// Check redirection
	QUrl redir = m_loadDetails->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
	if (!redir.isEmpty()) {
		m_pageUrl = m_parentSite->fixUrl(redir);
		log(QStringLiteral("Redirecting details page to `%1`").arg(m_pageUrl.toString()));
		loadDetails();
		return;
	}

	const int statusCode = m_loadDetails->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	if (statusCode == 429 || statusCode == 503 || statusCode == 509) {
		log(QStringLiteral("Details limit reached (HTTP %1). New try.").arg(statusCode));
		loadDetails(true);
		return;
	}

	// Detect Cloudflare
	if ((statusCode == 403 || statusCode == 429 || statusCode == 503) && m_loadDetails->rawHeader("server") == "cloudflare") {
		log(QStringLiteral("Cloudflare wall for '%1'").arg(m_pageUrl.toString()), Logger::Error);
		m_loadDetails->deleteLater();
		m_loadDetails = nullptr;
		emit finishedLoadingTags(LoadTagsResult::CloudflareError);
		return;
	}

	// Aborted or connection error
	if (m_loadDetails->error()) {
		if (m_loadDetails->error() != NetworkReply::NetworkError::OperationCanceledError) {
			log(QStringLiteral("Loading details error for '%1': %2").arg(m_pageUrl.toString(), m_loadDetails->errorString()), Logger::Error);
		}
		m_loadDetails->deleteLater();
		m_loadDetails = nullptr;
		emit finishedLoadingTags(LoadTagsResult::NetworkError);
		return;
	}

	const QString source = QString::fromUtf8(m_loadDetails->readAll());

	// Get an api able to parse details
	Api *api = m_parentSite->detailsApi();
	if (api == nullptr) {
		return;
	}

	// Parse source
	ParsedDetails ret = api->parseDetails(source, statusCode, m_parentSite);
	if (!ret.error.isEmpty()) {
		auto logLevel = m_detailsParsWarnAsErr ? Logger::Error : Logger::Warning;
		log(QStringLiteral("[%1][%2] %3").arg(m_parentSite->url(), api->getName(), ret.error), logLevel);
		m_loadDetails->deleteLater();
		m_loadDetails = nullptr;
		emit finishedLoadingTags(LoadTagsResult::Error);
		return;
	}

	// Fill data from parsing result
	if (!ret.pools.isEmpty()) {
		m_pools = ret.pools;
	}
	if (!ret.tags.isEmpty()) {
		m_tags = ret.tags;
	}
	if (ret.createdAt.isValid()) {
		m_data["date"] = ret.createdAt;
	}
	if (!ret.sources.isEmpty()) {
		m_sources = ret.sources;
	}

	// Image url
	if (!ret.imageUrl.isEmpty()) {
		const QUrl before = m_url;
		const QUrl newUrl = m_parentSite->fixUrl(ret.imageUrl, before);

		m_url = newUrl;
		m_sizes[Size::Full]->url = newUrl;

		delete m_extensionRotator;
		m_extensionRotator = nullptr;

		if (before != m_url) {
			if (getExtension(before) != getExtension(m_url)) {
				setFileSize(0, Size::Full);
			}
			emit urlChanged(before, m_url);
		}
	}

	m_loadDetails->deleteLater();
	m_loadDetails = nullptr;
	m_loadedDetails = true;

	refreshTokens();

	// If we load the details for an ugoira file that we will want to convert later, load the ugoira metadata as well
	if (extension() == QStringLiteral("zip") && m_settings->value("Save/ConvertUgoira", false).toBool()) {
		auto *endpoint = m_parentSite->apiEndpoint("ugoira_details");
		if (endpoint != nullptr) {
			const QString ugoiraDetailsUrl = endpoint->url(m_identity, 1, 1, {}, m_parentSite).url;

			log(QStringLiteral("Loading image ugoira details from `%1`").arg(ugoiraDetailsUrl), Logger::Info);
			auto *reply = m_parentSite->get(ugoiraDetailsUrl, Site::QueryType::Details);
			reply->setParent(this);

			connect(reply, &NetworkReply::finished, this, &Image::parseUgoiraDetails);
			return;
		}
	}

	emit finishedLoadingTags(LoadTagsResult::Ok);
}
void Image::parseUgoiraDetails()
{
	auto *reply = qobject_cast<NetworkReply*>(sender());
	auto *endpoint = m_parentSite->apiEndpoint("ugoira_details");

	// Handle network errors
	if (reply->error()) {
		if (reply->error() != NetworkReply::NetworkError::OperationCanceledError) {
			log(QStringLiteral("Loading ugoira details error for '%1': %2").arg(reply->url().toString(), reply->errorString()), Logger::Error);
		}
		reply->deleteLater();
		emit finishedLoadingTags(LoadTagsResult::NetworkError);
		return;
	}

	// Parse the metadata
	const QString source = QString::fromUtf8(reply->readAll());
	const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	m_data["ugoira_metadata"] = endpoint->parseAny(source, statusCode);

	reply->deleteLater();
	emit finishedLoadingTags(LoadTagsResult::Ok);
}

/**
 * Try to guess the size of the image in pixels for sorting.
 * @return The guessed number of pixels in the image.
 */
int Image::value() const
{
	QSize size = m_sizes[Image::Size::Full]->size;

	// Get from image size
	if (!size.isEmpty()) {
		return size.width() * size.height();
	}

	// Get from tags
	if (hasTag("incredibly_absurdres")) {
		return 10000 * 10000;
	}
	if (hasTag("absurdres")) {
		return 3200 * 2400;
	}
	if (hasTag("highres")) {
		return 1600 * 1200;
	}
	if (hasTag("lowres")) {
		return 500 * 500;
	}

	return 1200 * 900;
}

Image::SaveResult Image::preSave(const QString &path, Size size)
{
	bool force = false;

	// Check if file already exists on disk
	QFile f(path);
	if (f.exists() && !force) {
		return SaveResult::AlreadyExistsDisk;
	}

	// Check MD5 database
	const QPair<QString, QString> md5action = size != Size::Thumbnail
		? m_profile->md5Action(md5(), path)
		: QPair<QString, QString>("save", "");
	const QString &whatToDo = md5action.first;
	const QString &md5Duplicate = md5action.second;

	// Early return if this file shouldn't be saved (already exists in MD5 list and ignored)
	if (whatToDo == "ignore" && !force) {
		if (!QFile::exists(md5Duplicate)) {
			log(QStringLiteral("MD5 \"%1\" of the image `%2` already found in non-existing file `%3`").arg(md5(), m_url.toString(), md5Duplicate));
			return SaveResult::AlreadyExistsDeletedMd5;
		} else {
			log(QStringLiteral("MD5 \"%1\" of the image `%2` already found in file `%3`").arg(md5(), m_url.toString(), md5Duplicate));
			return SaveResult::AlreadyExistsMd5;
		}
	}

	// Create the destination directory since we're going to put a file there
	const QString p = path.section(QDir::separator(), 0, -2);
	QDir pathToFile(p), dir;
	if (!pathToFile.exists() && !dir.mkpath(p)) {
		log(QStringLiteral("Impossible to create the destination folder: %1.").arg(p), Logger::Error);
		return SaveResult::Error;
	}

	// Basic save action
	if (whatToDo == "save" || force) {
		const QString savePath = m_sizes[size]->save(path);
		if (savePath.isEmpty()) {
			return SaveResult::NotLoaded;
		}
		log(QStringLiteral("Saving image in `%1` (from `%2`)").arg(path, savePath));
		return SaveResult::Saved;
	}

	// Copy already existing file to the new path
	if (whatToDo == "copy") {
		log(QStringLiteral("Copy from `%1` to `%2`").arg(md5Duplicate, path));
		QFile(md5Duplicate).copy(path);
		return SaveResult::Copied;
	}

	// Move already existing file to the new path
	if (whatToDo == "move") {
		log(QStringLiteral("Moving from `%1` to `%2`").arg(md5Duplicate, path));
		QFile::rename(md5Duplicate, path);
		m_profile->removeMd5(md5(), md5Duplicate);
		return SaveResult::Moved;
	}

	// Create a shortcut/link to the existing file
	if (whatToDo == "link" || whatToDo == "hardlink") {
		log(QStringLiteral("Creating %1 for `%2` in `%3`").arg(whatToDo, md5Duplicate, path));
		createLink(md5Duplicate, path, whatToDo);
		#ifdef Q_OS_WIN
			if (whatToDo == "link") {
				return SaveResult::Shortcut;
			}
		#endif
		return SaveResult::Linked;
	}

	return SaveResult::Error;
}
QString &pathTokens(QString &filename, const QString &path)
{
	const QString &nativePath = QDir::toNativeSeparators(path);
	const QString &dir = QFileInfo(nativePath).absolutePath();
	return filename
		.replace("%path:nobackslash%", QString(nativePath).replace("\\", "/"))
		.replace("%path%", nativePath)
		.replace("%dir:nobackslash%", QString(dir).replace("\\", "/"))
		.replace("%dir%", dir);
}
QString Image::postSaving(const QString &originalPath, Size size, bool addMd5, bool startCommands, int count, bool basic)
{
	QString path = originalPath;

	// Save info to a text file
	if (!basic) {
		auto logFiles = getExternalLogFiles(m_settings);
		for (auto it = logFiles.constBegin(); it != logFiles.constEnd(); ++it) {
			auto logFile = it.value();
			const Filename textfileFormat = Filename(logFile["content"].toString());
			QStringList cont = textfileFormat.path(*this, m_profile, "", count, Filename::Complex);
			if (!cont.isEmpty()) {
				const int locationType = logFile["locationType"].toInt();
				QString contents = cont.first();

				// File path
				QString fileTagsPath;
				if (locationType == 0) {
					fileTagsPath = this->paths(logFile["filename"].toString(), logFile["path"].toString(), 0).first();
				} else if (locationType == 1) {
					fileTagsPath = logFile["uniquePath"].toString();
				} else if (locationType == 2) {
					fileTagsPath = path + logFile["suffix"].toString();
				} else if (locationType == 3) {
					fileTagsPath = setExtension(path, "") + logFile["suffixWithoutExtension"].toString();
				}

				// Replace some post-save tokens
				pathTokens(fileTagsPath, path);
				pathTokens(contents, path);

				// Append to file if necessary
				QFile fileTags(fileTagsPath);
				const bool append = fileTags.exists();
				if (fileTags.open(QFile::WriteOnly | QFile::Append | QFile::Text)) {
					if (append) {
						fileTags.write("\n");
					}
					fileTags.write(contents.toUtf8());
					fileTags.close();
				}
			}
		}
	}

	QString ext = extension();

	// Keep original date
	if (m_settings->value("Save/keepDate", true).toBool()) {
		setFileCreationDate(path, createdAt());
	}

	// Guess extension from file header
	if (m_settings->value("Save/headerDetection", true).toBool() && getExtension(path) == ext) {
		const QString headerExt = getExtensionFromHeader(path);
		if (!headerExt.isEmpty() && headerExt != ext) {
			log(QStringLiteral("Invalid file extension (%1 to %2) for `%3`").arg(ext, headerExt, path), Logger::Info);
			const QFileInfo info(path);
			const QString newPath = info.path() + QDir::separator() + info.completeBaseName() + "." + headerExt;
			if (!QFile::rename(path, newPath)) {
				log(QStringLiteral("Error renaming from `%1` to `%2`").arg(path, newPath), Logger::Error);
			} else {
				path = QDir::toNativeSeparators(newPath);
				ext = headerExt;
			}
		}
	}

	// Commands
	Commands &commands = m_profile->getCommands();
	if (startCommands) {
		commands.before();
	}
	for (const Tag &tag : qAsConst(m_tags)) {
		commands.tag(*this, tag, false);
	}
	commands.image(*this, path);
	for (const Tag &tag : qAsConst(m_tags)) {
		commands.tag(*this, tag, true);
	}
	if (startCommands) {
		commands.after();
	}

	// FFmpeg
	if (ext == QStringLiteral("webm")) {
		const bool remux = m_settings->value("Save/FFmpegRemuxWebmToMp4", false).toBool();
		const bool convert = m_settings->value("Save/FFmpegConvertWebmToMp4", false).toBool();
		const int timeout = m_settings->value("Save/FFmpegConvertTimeout", 30000).toInt();

		// We can only remux VP9 to MP4 as VP8 is not compatible with the MP4 container and needs conversion instead
		if (remux && FFmpeg::getVideoCodec(path) == QStringLiteral("vp9")) {
			path = FFmpeg::remux(path, "mp4", true, timeout);
			ext = getExtension(path);
		} else if (convert) {
			path = FFmpeg::convert(path, "mp4", true, timeout);
			ext = getExtension(path);
		}
	}

	// Image conversion
	const QString targetImgExt = m_settings->value("Save/ImageConversion/" + ext.toUpper() + "/to").toString().toLower();
	if (!targetImgExt.isEmpty()) {
		const QString backend = m_settings->value("Save/ImageConversionBackend", "ImageMagick").toString();
		const int timeout = m_settings->value("Save/ConvertUgoiraTimeout", 30000).toInt();
		if (backend == QStringLiteral("ImageMagick")) {
			path = ImageMagick::convert(path, targetImgExt, true, timeout);
		} else if (backend == QStringLiteral("FFmpeg")) {
			path = FFmpeg::convert(path, targetImgExt, true, timeout);
		}
		ext = getExtension(path);
	}

	// Ugoira conversion
	if (ext == QStringLiteral("zip") && m_settings->value("Save/ConvertUgoira", false).toBool()) {
		const QString targetUgoiraExt = m_settings->value("Save/ConvertUgoiraFormat", "gif").toString();
		const bool deleteOriginal = m_settings->value("Save/ConvertUgoiraDeleteOriginal", false).toBool();
		const int timeout = m_settings->value("Save/ConvertUgoiraTimeout", 30000).toInt();
		path = FFmpeg::convertUgoira(path, ugoiraFrameInformation(), targetUgoiraExt, deleteOriginal, timeout);
		ext = getExtension(path);
	}

	// Metadata
	#ifdef WIN_FILE_PROPS
		const QStringList exts = m_settings->value("Save/MetadataPropsysExtensions", "jpg jpeg mp4").toString().split(' ', Qt::SkipEmptyParts);
		if (exts.isEmpty() || exts.contains(ext)) {
			const auto metadataPropsys = getMetadataPropsys(m_settings);
			if (m_settings->value("Save/MetadataPropsysClear", false).toBool()) {
				clearAllWindowsProperties(path);
			}
			for (const auto &pair : metadataPropsys) {
				const QStringList values = Filename(pair.second).path(*this, m_profile, "", 0, Filename::Complex);
				if (!values.isEmpty()) {
					setWindowsProperty(path, pair.first, values.first());
				}
			}
		}
	#endif
	const QStringList exiftoolExts = m_settings->value("Save/MetadataExiftoolExtensions", "jpg jpeg png gif mp4").toString().split(' ', Qt::SkipEmptyParts);
	if (exiftoolExts.isEmpty() || exiftoolExts.contains(ext)) {
		QMap<QString, QString> metadata;
		const auto metadataExiftool = getMetadataExiftool(m_settings);
		for (const auto &pair : metadataExiftool) {
			const QStringList values = Filename(pair.second).path(*this, m_profile, "", 0, Filename::Complex);
			if (!values.isEmpty()) {
				metadata.insert(pair.first, values.first());
			}
		}

		if (!metadata.isEmpty()) {
			static const QMap<QString, Exiftool::SidecarFile> sidecarFileMapping {
				{"no", Exiftool::SidecarFile::No},
				{"on_error", Exiftool::SidecarFile::OnError},
				{"both", Exiftool::SidecarFile::Both},
				{"only", Exiftool::SidecarFile::Only},
			};
			Exiftool &exiftool = m_profile->getExiftool();
			exiftool.start();
			exiftool.setMetadata(
				path,
				metadata,
				m_settings->value("Save/MetadataExiftoolClear", false).toBool(),
				m_settings->value("Save/MetadataExiftoolKeepColorProfile", true).toBool(),
				sidecarFileMapping.value(m_settings->value("Save/MetadataExiftoolSidecar", "on_error").toString(), Exiftool::SidecarFile::OnError),
				m_settings->value("Save/MetadataExiftoolSidecarNoExtension", false).toBool()
			);
		}
	}

	if (addMd5) {
		m_profile->addMd5(md5(), path);
	}

	setSavePath(path, size);
	return path;
}


Site *Image::parentSite() const { return m_parentSite; }
const QList<Tag> &Image::tags() const { return m_tags; }
const QList<Pool> &Image::pools() const { return m_pools; }
qulonglong Image::id() const { return m_id; }
QVariantMap Image::identity(bool id) const
{
	if (m_identity.isEmpty() && id) {
		return {{"id", m_id}};
	}
	return m_identity;
}
int Image::fileSize() const { return m_sizes[Image::Size::Full]->fileSize; }
int Image::width() const { return size(Image::Size::Full).width(); }
int Image::height() const { return size(Image::Size::Full).height(); }
const QStringList &Image::search() const { return m_search; }
QDateTime Image::createdAt() const { return token<QDateTime>("date"); }
QString Image::dateRaw() const { return token<QString>("date_raw"); }
const QUrl &Image::fileUrl() const { return m_sizes[Size::Full]->url; }
const QUrl &Image::pageUrl() const { return m_pageUrl; }
QSize Image::size(Size size) const { return m_sizes[size]->size; }
QRect Image::rect(Size size) const { return m_sizes[size]->rect; }
const QString &Image::name() const { return m_name; }
QPixmap Image::previewImage() const { return m_sizes[Image::Size::Thumbnail]->pixmap(); }
const QPixmap &Image::previewImage() { return m_sizes[Image::Size::Thumbnail]->pixmap(); }
Page *Image::page() const { return m_parent; }
const QUrl &Image::parentUrl() const { return m_parentUrl; }
bool Image::isGallery() const { return m_isGallery; }
ExtensionRotator *Image::extensionRotator() const { return m_extensionRotator; }
QString Image::extension() const
{
	QString urlExt = getExtension(m_url).toLower();
	if (!urlExt.isEmpty()) {
		return urlExt;
	}
	return m_extension;
}

void Image::setPromoteDetailParsWarn(bool val) { m_detailsParsWarnAsErr = val; }
void Image::setPreviewImage(const QPixmap &preview)
{
	m_sizes[Image::Size::Thumbnail]->setPixmap(preview);
}
void Image::setTemporaryPath(const QString &path, Size size)
{
	if (m_sizes[size]->setTemporaryPath(path)) {
		refreshTokens();
	}
}
void Image::setSavePath(const QString &path, Size size)
{
	if (m_sizes[size]->setSavePath(path)) {
		refreshTokens();
	}
}
QString Image::savePath(Size size) const
{ return m_sizes[size]->savePath(); }

Image::Size Image::preferredDisplaySize() const
{
	const bool getOriginals = m_settings->value("Save/downloadoriginals", true).toBool();
	const bool viewSample = m_settings->value("Viewer/viewSamples", false).toBool();
	const bool isZip = getExtension(url(Size::Full)) == "zip";

	return !url(Size::Sample).isEmpty() && (!getOriginals || viewSample || isZip)
		? Size::Sample
		: Size::Full;
}

QStringList Image::tagsString(bool namespaces) const
{
	QStringList tags;
	tags.reserve(m_tags.count());
	for (const Tag &tag : m_tags) {
		const QString nspace = namespaces && !tag.type().isUnknown() ? tag.type().name() + ":" : QString();
		tags.append(nspace + tag.text());
	}
	return tags;
}

void Image::setUrl(const QUrl &url)
{
	setFileSize(0, Size::Full); // FIXME
	emit urlChanged(m_url, url);
	m_url = url;
	refreshTokens();
}
void Image::setSize(QSize size, Size s)
{
	m_sizes[s]->size = size;
	refreshTokens();
}
void Image::setFileSize(int fileSize, Size s)
{
	m_sizes[s]->fileSize = fileSize;
	refreshTokens();
}
void Image::setTags(const QList<Tag> &tags)
{
	m_tags = tags;
	refreshTokens();
}
void Image::setParentGallery(const QSharedPointer<Image> &parentGallery)
{
	m_parentGallery = parentGallery;
	if (m_search.isEmpty()) {
		m_search = m_parentGallery->search();
	}
	refreshTokens();
}

QColor Image::color() const
{
	// Blacklisted
	QStringList detected = m_profile->getBlacklist().match(tokens(m_profile));
	if (!detected.isEmpty()) {
		return QColor(m_settings->value("Coloring/Borders/blacklisteds", "#000000").toString());
	}

	// Favorited (except for exact favorite search)
	auto favorites = m_profile->getFavorites();
	for (const Tag &tag : m_tags) {
		if (!m_parent->search().contains(tag.text())) {
			for (const Favorite &fav : favorites) {
				if (fav.getName() == tag.text()) {
					return QColor(m_settings->value("Coloring/Borders/favorites", "#ffc0cb").toString());
				}
			}
		}
	}

	// Image with a parent
	if (token<int>("parentid") != 0) {
		return { 204, 204, 0 };
	}

	// Image with children
	if (token<bool>("has_children")) {
		return { 0, 255, 0 };
	}

	// Pending image
	if (token<QString>("status") == "pending") {
		return { 0, 0, 255 };
	}

	return {};
}

QString Image::tooltip() const
{
	double size = m_sizes[Image::Size::Full]->fileSize;
	const QString unit = getUnit(&size);

	const QString &rating = token<QString>("rating");
	const QDateTime &createdAt = token<QDateTime>("date");
	const QString &author = token<QString>("author");
	const QString &score = token<QString>("score");

	return QStringLiteral("%1%2%3%4%5%6%7%8%9")
		.arg(m_tags.isEmpty() ? " " : tr("<b>Tags:</b> %1<br/><br/>").arg(TagStylist(m_profile).stylished(m_tags, false, false, m_settings->value("Viewer/tagOrder", "type").toString()).join(' ')))
		.arg(m_id == 0 ? " " : tr("<b>ID:</b> %1<br/>").arg(m_id))
		.arg(m_name.isEmpty() ? " " : tr("<b>Name:</b> %1<br/>").arg(m_name))
		.arg(rating.isEmpty() ? " " : tr("<b>Rating:</b> %1<br/>").arg(rating))
		.arg(!score.isEmpty() ? tr("<b>Score:</b> %1<br/>").arg(score) : " ")
		.arg(author.isEmpty() ? " " : tr("<b>User:</b> %1<br/><br/>").arg(author))
		.arg(width() <= 0 || height() <= 0 ? " " : tr("<b>Size:</b> %1 x %2<br/>").arg(QString::number(width()), QString::number(height())))
		.arg(m_sizes[Image::Size::Full]->fileSize == 0 ? " " : tr("<b>Filesize:</b> %1 %2<br/>").arg(QString::number(size), unit))
		.arg(!createdAt.isValid() ? " " : tr("<b>Date:</b> %1").arg(QLocale().toString(createdAt.toLocalTime(), QLocale::ShortFormat)));
}

QString Image::counter() const
{
	return m_galleryCount > 0 ? QString::number(m_galleryCount) : (m_isGallery ? "?" : QString());
}

QList<QStrP> Image::detailsData() const
{
	const QString unknown = tr("<i>Unknown</i>");
	const QString yes = tr("yes");
	const QString no = tr("no");

	QString sources;
	for (const QString &source : m_sources) {
		sources += (!sources.isEmpty() ? "<br/>" : "") + QString("<a href=\"%1\">%1</a>").arg(source);
	}

	const QString &rating = token<QString>("rating");
	const QDateTime &createdAt = token<QDateTime>("date");
	const QString &author = token<QString>("author");
	int parentId = token<int>("parentid");

	return {
		QStrP(tr("Tags"), TagStylist(m_profile).stylished(m_tags, false, false, m_settings->value("Viewer/tagOrder", "type").toString()).join(' ')),
		QStrP(),
		QStrP(tr("ID"), m_id != 0 ? QString::number(m_id) : unknown),
		QStrP(tr("MD5"), !m_md5.isEmpty() ? m_md5 : unknown),
		QStrP(tr("Rating"), !rating.isEmpty() ? rating : unknown),
		QStrP(tr("Score"), token<QString>("score")),
		QStrP(tr("Author"), !author.isEmpty() ? author : unknown),
		QStrP(),
		QStrP(tr("Date"), createdAt.isValid() ? QLocale().toString(createdAt.toLocalTime(), QLocale::ShortFormat) : unknown),
		QStrP(tr("Size"), !size().isEmpty() ? QString::number(width()) + "x" + QString::number(height()) : unknown),
		QStrP(tr("Filesize"), m_sizes[Image::Size::Full]->fileSize != 0 ? formatFilesize(m_sizes[Image::Size::Full]->fileSize) : unknown),
		QStrP(),
		QStrP(tr("Page"), !m_pageUrl.isEmpty() ? QString("<a href=\"%1\">%1</a>").arg(m_pageUrl.toString()) : unknown),
		QStrP(tr("URL"), !m_sizes[Size::Full]->url.isEmpty() ? QString("<a href=\"%1\">%1</a>").arg(m_sizes[Size::Full]->url.toString()) : unknown),
		QStrP(tr("Source(s)", "", m_sources.count()), !sources.isEmpty() ? sources : unknown),
		QStrP(tr("Sample"), !url(Size::Sample).isEmpty() ? QString("<a href=\"%1\">%1</a>").arg(url(Size::Sample).toString()) : unknown),
		QStrP(tr("Thumbnail"), !url(Size::Thumbnail).isEmpty() ? QString("<a href=\"%1\">%1</a>").arg(url(Size::Thumbnail).toString()) : unknown),
		QStrP(),
		QStrP(tr("Parent"), parentId != 0 ? tr("yes (#%1)").arg(parentId) : no),
		QStrP(tr("Comments"), token<bool>("has_comments") ? yes : no),
		QStrP(tr("Children"), token<bool>("has_children") ? yes : no),
		QStrP(tr("Notes"), token<bool>("has_note") ? yes : no),
	};
}

QString Image::md5() const
{
	if (m_md5.isEmpty()) {
		return md5forced();
	}
	return m_md5;
}
QString Image::md5forced() const
{
	return m_sizes[Image::Size::Full]->md5();
}

bool Image::hasTag(QString tag) const
{
	tag = tag.trimmed();
	for (const Tag &t : m_tags) {
		if (QString::compare(t.text(), tag, Qt::CaseInsensitive) == 0) {
			return true;
		}
	}
	return false;
}
bool Image::hasUnknownTag() const
{
	if (m_tags.isEmpty()) {
		return true;
	}
	for (const Tag &tag : qAsConst(m_tags)) {
		if (tag.type().isUnknown()) {
			return true;
		}
	}
	return false;
}

void Image::setFileExtension(const QString &ext)
{
	m_url = setExtension(m_url, ext);
	m_sizes[Size::Full]->url = setExtension(m_sizes[Size::Full]->url, ext);
	refreshTokens();
}

bool Image::isVideo() const
{
	const QString ext = getExtension(m_url).toLower();
	return ext == "mp4" || ext == "webm";
}
QString Image::isAnimated() const
{
	QString ext = getExtension(m_url).toLower();

	if (ext == "gif" || ext == "apng") {
		return ext;
	}

	if (ext == "png" && (hasTag(QStringLiteral("animated")) || hasTag(QStringLiteral("animated_png")))) {
		return QStringLiteral("apng");
	}

	return QString();
}


QUrl Image::url(Size size) const
{
	if (size == Size::Full) {
		return m_url;
	}
	return m_sizes[size]->url;
}

void Image::preload(const Filename &filename)
{
	if (filename.needExactTags(m_parentSite, m_settings) == 0) {
		return;
	}

	QEventLoop loop;
	QObject::connect(this, &Image::finishedLoadingTags, &loop, &QEventLoop::quit);
	loadDetails();
	loop.exec();
}

QStringList Image::paths(const QString &filename, const QString &folder, int count) const
{
	return paths(Filename(filename), folder, count);
}
QStringList Image::paths(const Filename &filename, const QString &folder, int count) const
{
	return filename.path(*this, m_profile, folder, count, Filename::Complex | Filename::Path);
}

QMap<QString, Token> Image::generateTokens(Profile *profile) const
{
	const QSettings *settings = profile->getSettings();
	const QStringList &ignore = profile->getIgnored();
	const TagFilterList &remove = profile->getRemovedTags();

	QMap<QString, Token> tokens;
	QMap<QString, QStringList> details;

	// Pool
	static const QRegularExpression poolRegexp("pool:(\\d+)");
	QRegularExpressionMatch poolMatch = poolRegexp.match(m_search.join(' '));
	tokens.insert("pool", Token(poolMatch.hasMatch() ? poolMatch.captured(1) : "", ""));

	// Metadata
	tokens.insert("filename", Token(QUrl::fromPercentEncoding(m_url.fileName().section('.', 0, -2).toUtf8()), ""));
	tokens.insert("website", Token(m_parentSite->url()));
	tokens.insert("websitename", Token(m_parentSite->name()));
	tokens.insert("md5", Token(md5()));
	tokens.insert("md5_forced", Token([this]() { return this->md5forced(); }));
	tokens.insert("id", Token(m_id));
	tokens.insert("height", Token(height()));
	tokens.insert("width", Token(width()));
	tokens.insert("mpixels", Token(width() * height()));
	tokens.insert("ratio", Token(width() == height() ? 1 : static_cast<double>(width()) / static_cast<double>(height())));
	tokens.insert("url_file", Token(m_url));
	tokens.insert("url_original", Token(m_sizes[Size::Full]->url.toString()));
	tokens.insert("url_sample", Token(url(Size::Sample).toString()));
	tokens.insert("url_thumbnail", Token(url(Size::Thumbnail).toString()));
	tokens.insert("url_page", Token(m_pageUrl.toString()));
	tokens.insert("source", Token(!m_sources.isEmpty() ? m_sources.first() : ""));
	tokens.insert("sources", Token(m_sources));
	tokens.insert("filesize", Token(m_sizes[Image::Size::Full]->fileSize));
	tokens.insert("name", Token(m_name));
	tokens.insert("position", m_position > 0 ? Token(m_position) : Token(""));

	// Search
	for (int i = 0; i < m_search.size(); ++i) {
		tokens.insert("search_" + QString::number(i + 1), Token(m_search[i]));
	}
	for (int i = m_search.size(); i < 10; ++i) {
		tokens.insert("search_" + QString::number(i + 1), Token(""));
	}
	tokens.insert("search", Token(m_search.join(' ')));

	// Raw untouched tags (with underscores)
	for (const Tag &tag : m_tags) {
		details["allos"].append(QString(tag.text()).replace(' ', '_'));
	}

	// Tags
	const auto tags = remove.filterTags(m_tags);
	for (const Tag &tag : tags) {
		const QString &t = tag.text();

		details[ignore.contains(t, Qt::CaseInsensitive) ? "general" : tag.type().name()].append(t);
		details["alls"].append(t);
		details["alls_namespaces"].append(tag.type().name());
	}

	// Shorten copyrights
	if (settings->value("Save/copyright_useshorter", true).toBool()) {
		QStringList copyrights;
		for (const QString &cop : details["copyright"]) {
			bool found = false;
			for (QString &copyright : copyrights) {
				if (copyright.left(cop.size()) == cop.left(copyright.size())) {
					if (cop.size() < copyright.size()) {
						copyright = cop;
					}
					found = true;
				}
			}
			if (!found) {
				copyrights.append(cop);
			}
		}
		details["copyright"] = copyrights;
	}

	// Tags
	tokens.insert("general", Token(details["general"], "keepAll", "", ""));
	tokens.insert("artist", Token(details["artist"], "keepAll", "anonymous", "multiple artists"));
	tokens.insert("copyright", Token(details["copyright"], "keepAll", "misc", "crossover"));
	tokens.insert("character", Token(details["character"], "keepAll", "unknown", "group"));
	tokens.insert("model", Token(details["model"] + details["idol"], "keepAll", "unknown", "multiple"));
	tokens.insert("photo_set", Token(details["photo_set"], "keepAll", "unknown", "multiple"));
	tokens.insert("species", Token(details["species"], "keepAll", "unknown", "multiple"));
	tokens.insert("meta", Token(details["meta"], "keepAll", "none", "multiple"));
	tokens.insert("lore", Token(details["lore"], "keepAll", "none", "multiple"));
	tokens.insert("allos", Token(details["allos"]));
	tokens.insert("allo", Token(details["allos"].join(' ')));
	tokens.insert("tags", Token(QVariant::fromValue(tags)));
	tokens.insert("all", Token(details["alls"]));
	tokens.insert("all_namespaces", Token(details["alls_namespaces"]));

	// Extension
	QString ext = extension();
	if (settings->value("Save/noJpeg", true).toBool() && ext == "jpeg") {
		ext = "jpg";
	}
	tokens.insert("ext", Token(ext, "jpg"));
	tokens.insert("filetype", Token(ext, "jpg"));

	// Variables
	if (!m_parentGallery.isNull()) {
		tokens.insert("gallery", Token([this, profile]() { return QVariant::fromValue(m_parentGallery->tokens(profile)); }));
	}

	// Extra tokens
	static const QVariantMap defaultValues
	{
		{ "rating", "unknown" },
	};
	for (auto it = m_data.constBegin(); it != m_data.constEnd(); ++it) {
		tokens.insert(it.key(), Token(it.value(), defaultValues.value(it.key())));
	}
	for (auto it = defaultValues.constBegin(); it != defaultValues.constEnd(); ++it) {
		if (!tokens.contains(it.key())) {
			tokens.insert(it.key(), Token(it.value()));
		}
	}

	return tokens;
}

QString Image::postSave(const QString &path, Size size, SaveResult res, bool addMd5, bool startCommands, int count, bool basic)
{
	static const QList<SaveResult> md5Results { SaveResult::Moved, SaveResult::Copied, SaveResult::Shortcut, SaveResult::Linked, SaveResult::Saved };
	return postSaving(path, size, addMd5 && md5Results.contains(res), startCommands, count, basic);
}

bool Image::isValid() const
{
	return !url(Image::Size::Thumbnail).isEmpty()
		|| !m_name.isEmpty();
}

/**
 * Find the biggest media available in this image under the given size. Defaults to the thumbnail if none is found.
 *
 * @param size The bounding size not to exceed.
 * @param thumbnail If the media will be used as a thumbnail, its filetype should match the thumbnail filetype.
 * @return The biggest media available in this image under this size.
 */
const ImageSize &Image::mediaForSize(const QSize &size, bool thumbnail)
{
	QSharedPointer<ImageSize> ret;

	const QString thumbnailExt = getExtension(m_sizes[Size::Thumbnail]->url);

	// Find the biggest media smaller than the given size
	for (const QSharedPointer<ImageSize> &media : m_allSizes) {
		if (media->size.isValid() && media->size.width() <= size.width() && media->size.height() <= size.height() && (ret.isNull() || isBigger(media->size, ret->size))) {
			if (!thumbnail || getExtension(media->url) == thumbnailExt) {
				ret = media;
			}
		}
	}

	// Default to the thumbnail if no media was found
	if (ret.isNull()) {
		ret = m_sizes[Image::Thumbnail];
	}

	return *ret;
}

QList<QPair<QString, int>> Image::ugoiraFrameInformation() const
{
	// Ensure the ugoira metadata is loaded first
	const QVariant ugoiraMetadata = m_data.value("ugoira_metadata");
	if (!ugoiraMetadata.isValid() || ugoiraMetadata.isNull()) {
		return {};
	}

	QList<QPair<QString, int>> frameInformation;

	const auto frames = ugoiraMetadata.toMap()["frames"].toList();
	for (const QVariant &frame : frames) {
		const auto obj = frame.toMap();
		const QString file = obj["file"].isNull() ? "" : obj["file"].toString();
		frameInformation.append({file, obj["delay"].toInt()});
	}

	return frameInformation;
}
