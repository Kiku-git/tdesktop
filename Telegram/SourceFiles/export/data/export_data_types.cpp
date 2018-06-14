/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "export/data/export_data_types.h"

#include "core/mime_type.h"

#include <QtCore/QDateTime>
#include <QtCore/QRegularExpression>

namespace App { // Hackish..
QString formatPhone(QString phone);
} // namespace App
QString FillAmountAndCurrency(uint64 amount, const QString &currency);

namespace Export {
namespace Data {
namespace {

constexpr auto kUserPeerIdShift = (1ULL << 32);
constexpr auto kChatPeerIdShift = (2ULL << 32);

} // namespace

QString PreparePhotoFileName(TimeId date) {
	return "Photo_"
		+ QString::fromUtf8(FormatDateTime(date, '_', '_', '_'))
		+ ".jpg";
}

PeerId UserPeerId(int32 userId) {
	return kUserPeerIdShift | uint32(userId);
}

PeerId ChatPeerId(int32 chatId) {
	return kChatPeerIdShift | uint32(chatId);
}

int32 BarePeerId(PeerId peerId) {
	return int32(peerId & 0xFFFFFFFFULL);
}

PeerId ParsePeerId(const MTPPeer &data) {
	return data.match([](const MTPDpeerUser &data) {
		return UserPeerId(data.vuser_id.v);
	}, [](const MTPDpeerChat &data) {
		return ChatPeerId(data.vchat_id.v);
	}, [](const MTPDpeerChannel &data) {
		return ChatPeerId(data.vchannel_id.v);
	});
}

Utf8String ParseString(const MTPstring &data) {
	return data.v;
}

Utf8String FillLeft(const Utf8String &data, int length, char filler) {
	if (length <= data.size()) {
		return data;
	}
	auto result = Utf8String();
	result.reserve(length);
	for (auto i = 0, count = length - data.size(); i != count; ++i) {
		result.append(filler);
	}
	result.append(data);
	return result;
}

FileLocation ParseLocation(const MTPFileLocation &data) {
	return data.match([](const MTPDfileLocation &data) {
		return FileLocation{
			data.vdc_id.v,
			MTP_inputFileLocation(
				data.vvolume_id,
				data.vlocal_id,
				data.vsecret)
		};
	}, [](const MTPDfileLocationUnavailable &data) {
		return FileLocation{
			0,
			MTP_inputFileLocation(
				data.vvolume_id,
				data.vlocal_id,
				data.vsecret)
		};
	});
}

Image ParseMaxImage(
		const MTPVector<MTPPhotoSize> &data,
		const QString &suggestedPath) {
	auto result = Image();
	result.file.suggestedPath = suggestedPath;

	auto maxArea = int64(0);
	for (const auto &size : data.v) {
		size.match([](const MTPDphotoSizeEmpty &) {
		}, [&](const auto &data) {
			const auto area = data.vw.v * int64(data.vh.v);
			if (area > maxArea) {
				result.width = data.vw.v;
				result.height = data.vh.v;
				result.file.location = ParseLocation(data.vlocation);
				if constexpr (MTPDphotoCachedSize::Is<decltype(data)>()) {
					result.file.content = data.vbytes.v;
					result.file.size = result.file.content.size();
				} else {
					result.file.content = QByteArray();
					result.file.size = data.vsize.v;
				}
				maxArea = area;
			}
		});
	}
	return result;
}

Photo ParsePhoto(const MTPPhoto &data, const QString &suggestedPath) {
	auto result = Photo();
	data.match([&](const MTPDphoto &data) {
		result.id = data.vid.v;
		result.date = data.vdate.v;
		result.image = ParseMaxImage(data.vsizes, suggestedPath);
	}, [&](const MTPDphotoEmpty &data) {
		result.id = data.vid.v;
	});
	return result;
}

void ParseAttributes(
		Document &result,
		const MTPVector<MTPDocumentAttribute> &attributes) {
	for (const auto &value : attributes.v) {
		value.match([&](const MTPDdocumentAttributeImageSize &data) {
			result.width = data.vw.v;
			result.height = data.vh.v;
		}, [&](const MTPDdocumentAttributeAnimated &data) {
			result.isAnimated = true;
		}, [&](const MTPDdocumentAttributeSticker &data) {
			result.stickerEmoji = ParseString(data.valt);
		}, [&](const MTPDdocumentAttributeVideo &data) {
			if (data.is_round_message()) {
				result.isVideoMessage = true;
			} else {
				result.isVideoFile = true;
			}
			result.width = data.vw.v;
			result.height = data.vh.v;
			result.duration = data.vduration.v;
		}, [&](const MTPDdocumentAttributeAudio &data) {
			if (data.is_voice()) {
				result.isVoiceMessage = true;
			} else {
				result.isAudioFile = true;
			}
			result.songPerformer = ParseString(data.vperformer);
			result.songTitle = ParseString(data.vtitle);
			result.duration = data.vduration.v;
		}, [&](const MTPDdocumentAttributeFilename &data) {
			result.name = ParseString(data.vfile_name);
		}, [&](const MTPDdocumentAttributeHasStickers &data) {
		});
	}
}

QString ComputeDocumentName(const Document &data, TimeId date) {
	if (!data.name.isEmpty()) {
		return QString::fromUtf8(data.name);
	}
	const auto mimeString = QString::fromUtf8(data.mime);
	const auto mimeType = Core::MimeTypeForName(mimeString);
	const auto hasMimeType = [&](QLatin1String mime) {
		return !mimeString.compare(mime, Qt::CaseInsensitive);
	};
	const auto patterns = mimeType.globPatterns();
	auto pattern = patterns.isEmpty() ? QString() : patterns.front();
	auto extension = QString();
	auto prefix = QString();
	if (data.isVoiceMessage) {
		const auto isMP3 = hasMimeType(qstr("audio/mp3"));
		extension = isMP3 ? qsl(".mp3") : qsl(".ogg");
		prefix = qsl("Audio_");
	} else if (data.isVideoFile) {
		extension = pattern.isEmpty()
			? qsl(".mov")
			: QString(pattern).replace('*', QString());
		prefix = qsl("Video_");
	} else {
		extension = pattern.isEmpty()
			? qsl(".unknown")
			: pattern.replace('*', QString());
		prefix = qsl("File_");
	}
	return prefix
		+ QString::fromUtf8(FormatDateTime(date, '_', '_', '_'))
		+ extension;
}

QString CleanDocumentName(QString name) {
	// We don't want LTR/RTL mark/embedding/override/isolate chars
	// in filenames, because they introduce a security issue, when
	// an executable "Fil[x]gepj.exe" may look like "Filexe.jpeg".
	QChar controls[] = {
		0x200E, // LTR Mark
		0x200F, // RTL Mark
		0x202A, // LTR Embedding
		0x202B, // RTL Embedding
		0x202D, // LTR Override
		0x202E, // RTL Override
		0x2066, // LTR Isolate
		0x2067, // RTL Isolate
#ifdef Q_OS_WIN
		'\\',
		'/',
		':',
		'*',
		'?',
		'"',
		'<',
		'>',
		'|',
#elif defined Q_OS_MAC // Q_OS_WIN
		':',
#elif defined Q_OS_LINUX // Q_OS_WIN || Q_OS_MAC
		'/',
#endif // Q_OS_WIN || Q_OS_MAC || Q_OS_LINUX
	};
	for (const auto ch : controls) {
		name = std::move(name).replace(ch, '_');
	}

#ifdef Q_OS_WIN
	const auto lower = name.trimmed().toLower();
	const auto kBadExtensions = { qstr(".lnk"), qstr(".scf") };
	const auto kMaskExtension = qsl(".download");
	for (const auto extension : kBadExtensions) {
		if (lower.endsWith(extension)) {
			return name + kMaskExtension;
		}
	}
#endif // Q_OS_WIN

	return name;
}

Document ParseDocument(
		const MTPDocument &data,
		const QString &suggestedFolder,
		TimeId date) {
	auto result = Document();
	data.match([&](const MTPDdocument &data) {
		result.id = data.vid.v;
		result.date = data.vdate.v;
		result.file.size = data.vsize.v;
		result.file.location.dcId = data.vdc_id.v;
		result.file.location.data = MTP_inputDocumentFileLocation(
			data.vid,
			data.vaccess_hash,
			data.vversion);
		result.mime = ParseString(data.vmime_type);
		ParseAttributes(result, data.vattributes);
		result.file.suggestedPath = suggestedFolder
			+ CleanDocumentName(
				ComputeDocumentName(result, date ? date : result.date));
	}, [&](const MTPDdocumentEmpty &data) {
		result.id = data.vid.v;
	});
	return result;
}

GeoPoint ParseGeoPoint(const MTPGeoPoint &data) {
	auto result = GeoPoint();
	data.match([&](const MTPDgeoPoint &data) {
		result.latitude = data.vlat.v;
		result.longitude = data.vlong.v;
		result.valid = true;
	}, [](const MTPDgeoPointEmpty &data) {});
	return result;
}

Venue ParseVenue(const MTPDmessageMediaVenue &data) {
	auto result = Venue();
	result.point = ParseGeoPoint(data.vgeo);
	result.title = ParseString(data.vtitle);
	result.address = ParseString(data.vaddress);
	return result;
}

Game ParseGame(const MTPGame &data, int32 botId) {
	return data.match([&](const MTPDgame &data) {
		auto result = Game();
		result.id = data.vid.v;
		result.title = ParseString(data.vtitle);
		result.description = ParseString(data.vdescription);
		result.shortName = ParseString(data.vshort_name);
		result.botId = botId;
		return result;
	});
}

Invoice ParseInvoice(const MTPDmessageMediaInvoice &data) {
	auto result = Invoice();
	result.title = ParseString(data.vtitle);
	result.description = ParseString(data.vdescription);
	result.currency = ParseString(data.vcurrency);
	result.amount = data.vtotal_amount.v;
	if (data.has_receipt_msg_id()) {
		result.receiptMsgId = data.vreceipt_msg_id.v;
	}
	return result;
}

UserpicsSlice ParseUserpicsSlice(const MTPVector<MTPPhoto> &data) {
	const auto &list = data.v;
	auto result = UserpicsSlice();
	result.list.reserve(list.size());
	for (const auto &photo : list) {
		const auto suggestedPath = "PersonalPhotos/"
			+ (photo.type() == mtpc_photo
				? PreparePhotoFileName(photo.c_photo().vdate.v)
				: "Photo_Empty.jpg");
		result.list.push_back(ParsePhoto(photo, suggestedPath));
	}
	return result;
}

ContactInfo ParseContactInfo(const MTPUser &data) {
	auto result = ContactInfo();
	data.match([&](const MTPDuser &data) {
		result.userId = data.vid.v;
		if (data.has_first_name()) {
			result.firstName = ParseString(data.vfirst_name);
		}
		if (data.has_last_name()) {
			result.lastName = ParseString(data.vlast_name);
		}
		if (data.has_phone()) {
			result.phoneNumber = ParseString(data.vphone);
		}
	}, [&](const MTPDuserEmpty &data) {
		result.userId = data.vid.v;
	});
	return result;
}

ContactInfo ParseContactInfo(const MTPDmessageMediaContact &data) {
	auto result = ContactInfo();
	result.userId = data.vuser_id.v;
	result.firstName = ParseString(data.vfirst_name);
	result.lastName = ParseString(data.vlast_name);
	result.phoneNumber = ParseString(data.vphone_number);
	return result;
}

User ParseUser(const MTPUser &data) {
	auto result = User();
	result.info = ParseContactInfo(data);
	data.match([&](const MTPDuser &data) {
		if (data.has_username()) {
			result.username = ParseString(data.vusername);
		}
		if (data.has_bot_info_version()) {
			result.isBot = true;
		}
		const auto access_hash = data.has_access_hash()
			? data.vaccess_hash
			: MTP_long(0);
		result.input = MTP_inputUser(data.vid, access_hash);
	}, [&](const MTPDuserEmpty &data) {
		result.input = MTP_inputUser(data.vid, MTP_long(0));
	});
	return result;
}

std::map<int32, User> ParseUsersList(const MTPVector<MTPUser> &data) {
	auto result = std::map<int32, User>();
	for (const auto &user : data.v) {
		auto parsed = ParseUser(user);
		result.emplace(parsed.info.userId, std::move(parsed));
	}
	return result;
}

Chat ParseChat(const MTPChat &data) {
	auto result = Chat();
	data.match([&](const MTPDchat &data) {
		result.id = data.vid.v;
		result.title = ParseString(data.vtitle);
		result.input = MTP_inputPeerChat(MTP_int(result.id));
	}, [&](const MTPDchatEmpty &data) {
		result.id = data.vid.v;
		result.input = MTP_inputPeerChat(MTP_int(result.id));
	}, [&](const MTPDchatForbidden &data) {
		result.id = data.vid.v;
		result.title = ParseString(data.vtitle);
		result.input = MTP_inputPeerChat(MTP_int(result.id));
	}, [&](const MTPDchannel &data) {
		result.id = data.vid.v;
		result.broadcast = data.is_broadcast();
		result.title = ParseString(data.vtitle);
		if (data.has_username()) {
			result.username = ParseString(data.vusername);
		}
		result.input = MTP_inputPeerChannel(
			MTP_int(result.id),
			data.vaccess_hash);
	}, [&](const MTPDchannelForbidden &data) {
		result.id = data.vid.v;
		result.broadcast = data.is_broadcast();
		result.title = ParseString(data.vtitle);
		result.input = MTP_inputPeerChannel(
			MTP_int(result.id),
			data.vaccess_hash);
	});
	return result;
}

std::map<int32, Chat> ParseChatsList(const MTPVector<MTPChat> &data) {
	auto result = std::map<int32, Chat>();
	for (const auto &chat : data.v) {
		auto parsed = ParseChat(chat);
		result.emplace(parsed.id, std::move(parsed));
	}
	return result;
}

Utf8String ContactInfo::name() const {
	return firstName.isEmpty()
		? (lastName.isEmpty()
			? Utf8String()
			: lastName)
		: (lastName.isEmpty()
			? firstName
			: firstName + ' ' + lastName);

}

Utf8String User::name() const {
	return info.name();
}

const User *Peer::user() const {
	return base::get_if<User>(&data);
}
const Chat *Peer::chat() const {
	return base::get_if<Chat>(&data);
}

PeerId Peer::id() const {
	if (const auto user = this->user()) {
		return UserPeerId(user->info.userId);
	} else if (const auto chat = this->chat()) {
		return ChatPeerId(chat->id);
	}
	Unexpected("Variant in Peer::id.");
}

Utf8String Peer::name() const {
	if (const auto user = this->user()) {
		return user->name();
	} else if (const auto chat = this->chat()) {
		return chat->title;
	}
	Unexpected("Variant in Peer::id.");
}

MTPInputPeer Peer::input() const {
	if (const auto user = this->user()) {
		if (user->input.type() == mtpc_inputUser) {
			const auto &input = user->input.c_inputUser();
			return MTP_inputPeerUser(input.vuser_id, input.vaccess_hash);
		}
		return MTP_inputPeerEmpty();
	} else if (const auto chat = this->chat()) {
		return chat->input;
	}
	Unexpected("Variant in Peer::id.");
}

std::map<PeerId, Peer> ParsePeersLists(
		const MTPVector<MTPUser> &users,
		const MTPVector<MTPChat> &chats) {
	auto result = std::map<PeerId, Peer>();
	for (const auto &user : users.v) {
		auto parsed = ParseUser(user);
		result.emplace(
			UserPeerId(parsed.info.userId),
			Peer{ std::move(parsed) });
	}
	for (const auto &chat : chats.v) {
		auto parsed = ParseChat(chat);
		result.emplace(ChatPeerId(parsed.id), Peer{ std::move(parsed) });
	}
	return result;
}

File &Media::file() {
	return content.match([](Photo &data) -> File& {
		return data.image.file;
	}, [](Document &data) -> File& {
		return data.file;
	}, [](auto&) -> File& {
		static File result;
		return result;
	});
}

const File &Media::file() const {
	return content.match([](const Photo &data) -> const File& {
		return data.image.file;
	}, [](const Document &data) -> const File& {
		return data.file;
	}, [](const auto &) -> const File& {
		static const File result;
		return result;
	});
}

Media ParseMedia(
		const MTPMessageMedia &data,
		const QString &folder,
		TimeId date,
		int32 botId) {
	Expects(folder.isEmpty() || folder.endsWith(QChar('/')));

	auto result = Media();
	data.match([&](const MTPDmessageMediaPhoto &data) {
		result.content = data.has_photo()
			? ParsePhoto(
				data.vphoto,
				folder + "Photos/" + PreparePhotoFileName(date))
			: Photo();
		if (data.has_ttl_seconds()) {
			result.ttl = data.vttl_seconds.v;
		}
	}, [&](const MTPDmessageMediaGeo &data) {
		result.content = ParseGeoPoint(data.vgeo);
	}, [&](const MTPDmessageMediaContact &data) {
		result.content = ParseContactInfo(data);
	}, [&](const MTPDmessageMediaUnsupported &data) {
		result.content = UnsupportedMedia();
	}, [&](const MTPDmessageMediaDocument &data) {
		result.content = data.has_document()
			? ParseDocument(
				data.vdocument,
				folder + "Files/",
				date)
			: Document();
		if (data.has_ttl_seconds()) {
			result.ttl = data.vttl_seconds.v;
		}
	}, [&](const MTPDmessageMediaWebPage &data) {
		// Ignore web pages.
	}, [&](const MTPDmessageMediaVenue &data) {
		result.content = ParseVenue(data);
	}, [&](const MTPDmessageMediaGame &data) {
		result.content = ParseGame(data.vgame, botId);
	}, [&](const MTPDmessageMediaInvoice &data) {
		result.content = ParseInvoice(data);
	}, [&](const MTPDmessageMediaGeoLive &data) {
		result.content = ParseGeoPoint(data.vgeo);
		result.ttl = data.vperiod.v;
	}, [](const MTPDmessageMediaEmpty &data) {});
	return result;
}

ServiceAction ParseServiceAction(
		const MTPMessageAction &data,
		const QString &mediaFolder,
		TimeId date) {
	auto result = ServiceAction();
	data.match([&](const MTPDmessageActionChatCreate &data) {
		auto content = ActionChatCreate();
		content.title = ParseString(data.vtitle);
		content.userIds.reserve(data.vusers.v.size());
		for (const auto &userId : data.vusers.v) {
			content.userIds.push_back(userId.v);
		}
		result.content = content;
	}, [&](const MTPDmessageActionChatEditTitle &data) {
		auto content = ActionChatEditTitle();
		content.title = ParseString(data.vtitle);
		result.content = content;
	}, [&](const MTPDmessageActionChatEditPhoto &data) {
		auto content = ActionChatEditPhoto();
		content.photo = ParsePhoto(
			data.vphoto,
			mediaFolder + "Photos/" + PreparePhotoFileName(date));
		result.content = content;
	}, [&](const MTPDmessageActionChatDeletePhoto &data) {
		result.content = ActionChatDeletePhoto();
	}, [&](const MTPDmessageActionChatAddUser &data) {
		auto content = ActionChatAddUser();
		content.userIds.reserve(data.vusers.v.size());
		for (const auto &user : data.vusers.v) {
			content.userIds.push_back(user.v);
		}
		result.content = content;
	}, [&](const MTPDmessageActionChatDeleteUser &data) {
		auto content = ActionChatDeleteUser();
		content.userId = data.vuser_id.v;
		result.content = content;
	}, [&](const MTPDmessageActionChatJoinedByLink &data) {
		auto content = ActionChatJoinedByLink();
		content.inviterId = data.vinviter_id.v;
		result.content = content;
	}, [&](const MTPDmessageActionChannelCreate &data) {
		auto content = ActionChannelCreate();
		content.title = ParseString(data.vtitle);
		result.content = content;
	}, [&](const MTPDmessageActionChatMigrateTo &data) {
		auto content = ActionChatMigrateTo();
		content.channelId = data.vchannel_id.v;
		result.content = content;
	}, [&](const MTPDmessageActionChannelMigrateFrom &data) {
		auto content = ActionChannelMigrateFrom();
		content.title = ParseString(data.vtitle);
		content.chatId = data.vchat_id.v;
		result.content = content;
	}, [&](const MTPDmessageActionPinMessage &data) {
		result.content = ActionPinMessage();
	}, [&](const MTPDmessageActionHistoryClear &data) {
		result.content = ActionHistoryClear();
	}, [&](const MTPDmessageActionGameScore &data) {
		auto content = ActionGameScore();
		content.gameId = data.vgame_id.v;
		content.score = data.vscore.v;
		result.content = content;
	}, [&](const MTPDmessageActionPaymentSentMe &data) {
		// Should not be in user inbox.
	}, [&](const MTPDmessageActionPaymentSent &data) {
		auto content = ActionPaymentSent();
		content.currency = ParseString(data.vcurrency);
		content.amount = data.vtotal_amount.v;
		result.content = content;
	}, [&](const MTPDmessageActionPhoneCall &data) {
		auto content = ActionPhoneCall();
		if (data.has_duration()) {
			content.duration = data.vduration.v;
		}
		if (data.has_reason()) {
			using Reason = ActionPhoneCall::DiscardReason;
			content.discardReason = data.vreason.match(
			[](const MTPDphoneCallDiscardReasonMissed &data) {
				return Reason::Missed;
			}, [](const MTPDphoneCallDiscardReasonDisconnect &data) {
				return Reason::Disconnect;
			}, [](const MTPDphoneCallDiscardReasonHangup &data) {
				return Reason::Hangup;
			}, [](const MTPDphoneCallDiscardReasonBusy &data) {
				return Reason::Busy;
			});
		}
		result.content = content;
	}, [&](const MTPDmessageActionScreenshotTaken &data) {
		result.content = ActionScreenshotTaken();
	}, [&](const MTPDmessageActionCustomAction &data) {
		auto content = ActionCustomAction();
		content.message = ParseString(data.vmessage);
		result.content = content;
	}, [&](const MTPDmessageActionBotAllowed &data) {
		auto content = ActionBotAllowed();
		content.domain = ParseString(data.vdomain);
		result.content = content;
	}, [&](const MTPDmessageActionSecureValuesSentMe &data) {
		// Should not be in user inbox.
	}, [&](const MTPDmessageActionSecureValuesSent &data) {
		auto content = ActionSecureValuesSent();
		content.types.reserve(data.vtypes.v.size());
		using Type = ActionSecureValuesSent::Type;
		for (const auto &type : data.vtypes.v) {
			content.types.push_back(type.match(
			[](const MTPDsecureValueTypePersonalDetails &data) {
				return Type::PersonalDetails;
			}, [](const MTPDsecureValueTypePassport &data) {
				return Type::Passport;
			}, [](const MTPDsecureValueTypeDriverLicense &data) {
				return Type::DriverLicense;
			}, [](const MTPDsecureValueTypeIdentityCard &data) {
				return Type::IdentityCard;
			}, [](const MTPDsecureValueTypeInternalPassport &data) {
				return Type::InternalPassport;
			}, [](const MTPDsecureValueTypeAddress &data) {
				return Type::Address;
			}, [](const MTPDsecureValueTypeUtilityBill &data) {
				return Type::UtilityBill;
			}, [](const MTPDsecureValueTypeBankStatement &data) {
				return Type::BankStatement;
			}, [](const MTPDsecureValueTypeRentalAgreement &data) {
				return Type::RentalAgreement;
			}, [](const MTPDsecureValueTypePassportRegistration &data) {
				return Type::PassportRegistration;
			}, [](const MTPDsecureValueTypeTemporaryRegistration &data) {
				return Type::TemporaryRegistration;
			}, [](const MTPDsecureValueTypePhone &data) {
				return Type::Phone;
			}, [](const MTPDsecureValueTypeEmail &data) {
				return Type::Email;
			}));
		}
		result.content = content;
	}, [](const MTPDmessageActionEmpty &data) {});
	return result;
}

File &Message::file() {
	const auto service = &action.content;
	if (const auto photo = base::get_if<ActionChatEditPhoto>(service)) {
		return photo->photo.image.file;
	}
	return media.file();
}

const File &Message::file() const {
	const auto service = &action.content;
	if (const auto photo = base::get_if<ActionChatEditPhoto>(service)) {
		return photo->photo.image.file;
	}
	return media.file();
}

Message ParseMessage(const MTPMessage &data, const QString &mediaFolder) {
	auto result = Message();
	data.match([&](const MTPDmessage &data) {
		result.id = data.vid.v;
		const auto date = result.date = data.vdate.v;
		if (data.has_edit_date()) {
			result.edited = data.vedit_date.v;
		}
		if (data.has_from_id()) {
			result.fromId = data.vfrom_id.v;
		}
		if (data.has_fwd_from()) {
			result.forwardedFromId = data.vfwd_from.match(
			[](const MTPDmessageFwdHeader &data) {
				if (data.has_channel_id()) {
					return ChatPeerId(data.vchannel_id.v);
				} else if (data.has_saved_from_peer()) {
					return ParsePeerId(data.vsaved_from_peer);
				} else if (data.has_from_id()) {
					return UserPeerId(data.vfrom_id.v);
				}
				return PeerId(0);
			});
		}
		if (data.has_post_author()) {
			result.signature = ParseString(data.vpost_author);
		}
		if (data.has_reply_to_msg_id()) {
			result.replyToMsgId = data.vreply_to_msg_id.v;
		}
		if (data.has_via_bot_id()) {
			result.viaBotId = data.vvia_bot_id.v;
		}
		if (data.has_media()) {
			result.media = ParseMedia(
				data.vmedia,
				mediaFolder,
				date,
				(result.viaBotId
					? result.viaBotId
					: result.forwardedFromId
					? result.forwardedFromId
					: result.fromId));
		}
		result.text = ParseString(data.vmessage);
	}, [&](const MTPDmessageService &data) {
		result.id = data.vid.v;
		const auto date = result.date = data.vdate.v;
		result.action = ParseServiceAction(data.vaction, mediaFolder, date);
		if (data.has_from_id()) {
			result.fromId = data.vfrom_id.v;
		}
		if (data.has_reply_to_msg_id()) {
			result.replyToMsgId = data.vreply_to_msg_id.v;
		}
	}, [&](const MTPDmessageEmpty &data) {
		result.id = data.vid.v;
	});
	return result;
}

std::map<int32, Message> ParseMessagesList(
		const MTPVector<MTPMessage> &data,
		const QString &mediaFolder) {
	auto result = std::map<int32, Message>();
	for (const auto &message : data.v) {
		auto parsed = ParseMessage(message, mediaFolder);
		result.emplace(parsed.id, std::move(parsed));
	}
	return result;
}

PersonalInfo ParsePersonalInfo(const MTPUserFull &data) {
	Expects(data.type() == mtpc_userFull);

	const auto &fields = data.c_userFull();
	auto result = PersonalInfo();
	result.user = ParseUser(fields.vuser);
	if (fields.has_about()) {
		result.bio = ParseString(fields.vabout);
	}
	return result;
}

ContactsList ParseContactsList(const MTPcontacts_Contacts &data) {
	Expects(data.type() == mtpc_contacts_contacts);

	auto result = ContactsList();
	const auto &contacts = data.c_contacts_contacts();
	const auto map = ParseUsersList(contacts.vusers);
	result.list.reserve(contacts.vcontacts.v.size());
	for (const auto &contact : contacts.vcontacts.v) {
		const auto userId = contact.c_contact().vuser_id.v;
		if (const auto i = map.find(userId); i != end(map)) {
			result.list.push_back(i->second.info);
		} else {
			result.list.push_back(ContactInfo());
		}
	}
	return result;
}

std::vector<int> SortedContactsIndices(const ContactsList &data) {
	const auto names = ranges::view::all(
		data.list
	) | ranges::view::transform([](const Data::ContactInfo &info) {
		return (QString::fromUtf8(info.firstName)
			+ ' '
			+ QString::fromUtf8(info.lastName)).toLower();
	}) | ranges::to_vector;

	auto indices = ranges::view::ints(0, int(data.list.size()))
		| ranges::to_vector;
	ranges::sort(indices, [&](int i, int j) {
		return names[i] < names[j];
	});
	return indices;
}

Session ParseSession(const MTPAuthorization &data) {
	Expects(data.type() == mtpc_authorization);

	const auto &fields = data.c_authorization();
	auto result = Session();
	result.platform = ParseString(fields.vplatform);
	result.deviceModel = ParseString(fields.vdevice_model);
	result.systemVersion = ParseString(fields.vsystem_version);
	result.applicationName = ParseString(fields.vapp_name);
	result.applicationVersion = ParseString(fields.vapp_version);
	result.created = fields.vdate_created.v;
	result.lastActive = fields.vdate_active.v;
	result.ip = ParseString(fields.vip);
	result.country = ParseString(fields.vcountry);
	result.region = ParseString(fields.vregion);
	return result;
}

SessionsList ParseSessionsList(const MTPaccount_Authorizations &data) {
	Expects(data.type() == mtpc_account_authorizations);

	auto result = SessionsList();
	const auto &list = data.c_account_authorizations().vauthorizations.v;
	result.list.reserve(list.size());
	for (const auto &session : list) {
		result.list.push_back(ParseSession(session));
	}
	return result;
}

DialogsInfo ParseDialogsInfo(const MTPmessages_Dialogs &data) {
	auto result = DialogsInfo();
	const auto folder = QString();
	data.match([&](const auto &data) { // MTPDmessages_dialogs &data) {
		const auto peers = ParsePeersLists(data.vusers, data.vchats);
		const auto messages = ParseMessagesList(data.vmessages, folder);
		result.list.reserve(result.list.size() + data.vdialogs.v.size());
		for (const auto &dialog : data.vdialogs.v) {
			if (dialog.type() != mtpc_dialog) {
				continue;
			}
			const auto &fields = dialog.c_dialog();

			auto info = DialogInfo();
			const auto peerId = ParsePeerId(fields.vpeer);
			const auto peerIt = peers.find(peerId);
			if (peerIt != end(peers)) {
				const auto &peer = peerIt->second;
				info.type = peer.user()
					? DialogInfo::Type::Personal
					: peer.chat()->broadcast
					? DialogInfo::Type::Channel
					: peer.chat()->username.isEmpty()
					? DialogInfo::Type::PrivateGroup
					: DialogInfo::Type::PublicGroup;
				info.name = peer.name();
				info.input = peer.input();
			}
			info.topMessageId = fields.vtop_message.v;
			const auto messageIt = messages.find(info.topMessageId);
			if (messageIt != end(messages)) {
				const auto &message = messageIt->second;
				info.topMessageDate = message.date;
			}
			result.list.push_back(std::move(info));
		}
	});
	return result;
}

MessagesSlice ParseMessagesSlice(
		const MTPVector<MTPMessage> &data,
		const MTPVector<MTPUser> &users,
		const MTPVector<MTPChat> &chats,
		const QString &mediaFolder) {
	const auto &list = data.v;
	auto result = MessagesSlice();
	result.list.reserve(list.size());
	for (const auto &message : list) {
		result.list.push_back(ParseMessage(message, mediaFolder));
	}
	ranges::reverse(result.list);
	result.peers = ParsePeersLists(users, chats);
	return result;
}

Utf8String FormatPhoneNumber(const Utf8String &phoneNumber) {
	return phoneNumber.isEmpty()
		? Utf8String()
		: App::formatPhone(QString::fromUtf8(phoneNumber)).toUtf8();
}

Utf8String FormatDateTime(
		TimeId date,
		QChar dateSeparator,
		QChar timeSeparator,
		QChar separator) {
	const auto value = QDateTime::fromTime_t(date);
	return (QString("%1") + dateSeparator + "%2" + dateSeparator + "%3"
		+ separator + "%4" + timeSeparator + "%5" + timeSeparator + "%6"
	).arg(value.date().year()
	).arg(value.date().month(), 2, 10, QChar('0')
	).arg(value.date().day(), 2, 10, QChar('0')
	).arg(value.time().hour(), 2, 10, QChar('0')
	).arg(value.time().minute(), 2, 10, QChar('0')
	).arg(value.time().second(), 2, 10, QChar('0')
	).toUtf8();
}

Utf8String FormatMoneyAmount(uint64 amount, const Utf8String &currency) {
	return FillAmountAndCurrency(
		amount,
		QString::fromUtf8(currency)).toUtf8();
}

} // namespace Data
} // namespace Export
