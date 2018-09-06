/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/settings_main.h"

#include "settings/settings_common.h"
#include "settings/settings_codes.h"
#include "boxes/abstract_box.h"
#include "boxes/language_box.h"
#include "boxes/confirm_box.h"
#include "boxes/about_box.h"
#include "boxes/photo_crop_box.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/discrete_sliders.h"
#include "info/profile/info_profile_button.h"
#include "info/profile/info_profile_cover.h"
#include "lang/lang_keys.h"
#include "storage/localstorage.h"
#include "auth_session.h"
#include "apiwrap.h"
#include "core/file_utilities.h"
#include "styles/style_settings.h"

namespace Settings {
namespace {

void AddSkip(not_null<Ui::VerticalLayout*> container) {
	container->add(object_ptr<Ui::FixedHeightWidget>(
		container,
		st::settingsSectionSkip));
}

void AddDivider(not_null<Ui::VerticalLayout*> container) {
	container->add(object_ptr<BoxContentDivider>(container));
}

void SetupUploadPhotoButton(
		not_null<Ui::VerticalLayout*> container,
		not_null<UserData*> self) {
	AddDivider(container);
	AddSkip(container);

	const auto upload = [=] {
		const auto imageExtensions = cImgExtensions();
		const auto filter = qsl("Image files (*")
			+ imageExtensions.join(qsl(" *"))
			+ qsl(");;")
			+ FileDialog::AllFilesFilter();
		const auto callback = [=](const FileDialog::OpenResult &result) {
			if (result.paths.isEmpty() && result.remoteContent.isEmpty()) {
				return;
			}

			const auto image = result.remoteContent.isEmpty()
				? App::readImage(result.paths.front())
				: App::readImage(result.remoteContent);
			if (image.isNull()
				|| image.width() > 10 * image.height()
				|| image.height() > 10 * image.width()) {
				Ui::show(Box<InformBox>(lang(lng_bad_photo)));
				return;
			}

			auto box = Ui::show(Box<PhotoCropBox>(image, self));
			box->ready(
			) | rpl::start_with_next([=](QImage &&image) {
				Auth().api().uploadPeerPhoto(self, std::move(image));
			}, box->lifetime());
		};
		FileDialog::GetOpenPath(
			container.get(),
			lang(lng_choose_image),
			filter,
			crl::guard(container, callback));
	};
	container->add(object_ptr<Button>(
		container,
		Lang::Viewer(lng_settings_upload),
		st::settingsSectionButton)
	)->addClickHandler(App::LambdaDelayed(
		st::settingsSectionButton.ripple.hideDuration,
		container,
		upload));

	AddSkip(container);
}

void SetupLanguageButton(not_null<Ui::VerticalLayout*> container) {
	const auto button = container->add(object_ptr<Button>(
		container,
		Lang::Viewer(lng_settings_language),
		st::settingsSectionButton));
	const auto guard = Ui::AttachAsChild(button, base::binary_guard());
	button->addClickHandler([=] {
		*guard = LanguageBox::Show();
	});
	const auto name = Ui::CreateChild<Ui::FlatLabel>(
		button,
		Lang::Viewer(lng_language_name),
		st::settingsButtonRight);
	rpl::combine(
		name->widthValue(),
		button->widthValue()
	) | rpl::start_with_next([=] {
		name->moveToRight(
			st::settingsButtonRightPosition.x(),
			st::settingsButtonRightPosition.y());
	}, name->lifetime());
}

void SetupSections(
		not_null<Ui::VerticalLayout*> container,
		Fn<void(Type)> showOther) {
	AddDivider(container);
	AddSkip(container);

	const auto addSection = [&](LangKey label, Type type) {
		container->add(object_ptr<Button>(
			container,
			Lang::Viewer(label),
			st::settingsSectionButton)
		)->addClickHandler([=] { showOther(type); });
	};
	addSection(lng_settings_section_info, Type::Information);
	addSection(lng_settings_section_notify, Type::Notifications);
	addSection(lng_settings_section_privacy, Type::PrivacySecurity);
	addSection(lng_settings_section_general, Type::General);
	addSection(lng_settings_section_chat_settings, Type::Chat);

	SetupLanguageButton(container);

	AddSkip(container);
}

void SetupInterfaceScale(not_null<Ui::VerticalLayout*> container) {
	if (cRetina()) {
		return;
	}
	AddDivider(container);
	AddSkip(container);

	const auto toggled = Ui::AttachAsChild(
		container,
		rpl::event_stream<bool>());

	const auto switched = (cConfigScale() == dbisAuto)
		|| (cConfigScale() == cScreenScale());
	const auto button = container->add(object_ptr<Button>(
		container,
		Lang::Viewer(lng_settings_default_scale),
		st::settingsSectionButton)
	)->toggleOn(toggled->events_starting_with_copy(switched));

	const auto slider = container->add(
		object_ptr<Ui::SettingsSlider>(container, st::settingsSlider),
		st::settingsScalePadding);

	const auto inSetScale = Ui::AttachAsChild(container, false);
	const auto setScale = std::make_shared<Fn<void(DBIScale)>>();
	*setScale = [=](DBIScale scale) {
		if (*inSetScale) return;
		*inSetScale = true;
		const auto guard = gsl::finally([=] { *inSetScale = false; });

		if (scale == cScreenScale()) {
			scale = dbisAuto;
		}
		toggled->fire(scale == dbisAuto);
		const auto applying = scale;
		if (scale == dbisAuto) {
			scale = cScreenScale();
		}
		slider->setActiveSection(scale - 1);

		if (cEvalScale(scale) != cEvalScale(cRealScale())) {
			const auto confirmed = crl::guard(button, [=] {
				cSetConfigScale(applying);
				Local::writeSettings();
				App::restart();
			});
			const auto cancelled = crl::guard(button, [=] {
				App::CallDelayed(
					st::defaultSettingsSlider.duration,
					button,
					[=] { (*setScale)(cRealScale()); });
			});
			Ui::show(Box<ConfirmBox>(
				lang(lng_settings_need_restart),
				lang(lng_settings_restart_now),
				confirmed,
				cancelled));
		} else {
			cSetConfigScale(scale);
			Local::writeSettings();
		}
	};
	button->toggledValue(
	) | rpl::start_with_next([=](bool checked) {
		auto scale = checked ? dbisAuto : cEvalScale(cConfigScale());
		if (scale == cScreenScale()) {
			if (scale != cScale()) {
				scale = cScale();
			} else {
				switch (scale) {
				case dbisOne: scale = dbisOneAndQuarter; break;
				case dbisOneAndQuarter: scale = dbisOne; break;
				case dbisOneAndHalf: scale = dbisOneAndQuarter; break;
				case dbisTwo: scale = dbisOneAndHalf; break;
				}
			}
		}
		(*setScale)(scale);
	}, button->lifetime());

	const auto label = [](DBIScale scale) {
		switch (scale) {
		case dbisOne: return qsl("100%");
		case dbisOneAndQuarter: return qsl("125%");
		case dbisOneAndHalf: return qsl("150%");
		case dbisTwo: return qsl("200%");
		}
		Unexpected("Value in scale label.");
	};
	const auto scaleByIndex = [](int index) {
		switch (index) {
		case 0: return dbisOne;
		case 1: return dbisOneAndQuarter;
		case 2: return dbisOneAndHalf;
		case 3: return dbisTwo;
		}
		Unexpected("Index in scaleByIndex.");
	};

	slider->addSection(label(dbisOne));
	slider->addSection(label(dbisOneAndQuarter));
	slider->addSection(label(dbisOneAndHalf));
	slider->addSection(label(dbisTwo));
	slider->setActiveSectionFast(cEvalScale(cConfigScale()) - 1);
	slider->sectionActivated(
	) | rpl::start_with_next([=](int section) {
		(*setScale)(scaleByIndex(section));
	}, slider->lifetime());

	AddSkip(container);
}

void SetupHelp(not_null<Ui::VerticalLayout*> container) {
	AddDivider(container);
	AddSkip(container);

	container->add(object_ptr<Button>(
		container,
		Lang::Viewer(lng_settings_faq),
		st::settingsSectionButton)
	)->addClickHandler([] {
		QDesktopServices::openUrl(telegramFaqLink());
	});

	if (AuthSession::Exists()) {
		const auto button = container->add(object_ptr<Button>(
			container,
			Lang::Viewer(lng_settings_ask_question),
			st::settingsSectionButton));
		button->addClickHandler([=] {
			const auto ready = crl::guard(button, [](const MTPUser &data) {
				const auto users = MTP_vector<MTPUser>(1, data);
				if (const auto user = App::feedUsers(users)) {
					Ui::showPeerHistory(user, ShowAtUnreadMsgId);
				}
			});
			Auth().api().requestSupportContact(ready);
		});
	}

	AddSkip(container);
}

} // namespace

Main::Main(
	QWidget *parent,
	not_null<Window::Controller*> controller,
	not_null<UserData*> self)
: Section(parent)
, _self(self) {
	setupContent(controller);
}

void Main::keyPressEvent(QKeyEvent *e) {
	CodesFeedString(e->text());
	return Section::keyPressEvent(e);
}

void Main::setupContent(not_null<Window::Controller*> controller) {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);

	const auto cover = content->add(object_ptr<Info::Profile::Cover>(
		content,
		_self,
		controller));
	cover->setOnlineCount(rpl::single(0));

	SetupUploadPhotoButton(content, _self);
	SetupSections(content, [=](Type type) {
		_showOther.fire_copy(type);
	});
	SetupInterfaceScale(content);
	SetupHelp(content);

	Ui::ResizeFitChild(this, content);
}

rpl::producer<Type> Main::sectionShowOther() {
	return _showOther.events();
}

} // namespace Settings
