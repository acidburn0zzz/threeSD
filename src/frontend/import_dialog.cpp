// Copyright 2019 threeSD Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <unordered_map>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QProgressBar>
#include <QProgressDialog>
#include <QStorageInfo>
#include <QtConcurrent/QtConcurrentRun>
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/progress_callback.h"
#include "common/scope_exit.h"
#include "frontend/cia_build_dialog.h"
#include "frontend/helpers/frontend_common.h"
#include "frontend/helpers/multi_job.h"
#include "frontend/helpers/simple_job.h"
#include "frontend/import_dialog.h"
#include "frontend/title_info_dialog.h"
#include "ui_import_dialog.h"

// content type, singular name, plural name, icon name
// clang-format off
static constexpr std::array<std::tuple<Core::ContentType, const char*, const char*, const char*>, 9>
    ContentTypeMap{{
        {Core::ContentType::Application, QT_TR_NOOP("Application"), QT_TR_NOOP("Applications"), "app"},
        {Core::ContentType::Update, QT_TR_NOOP("Update"),  QT_TR_NOOP("Updates"), "update"},
        {Core::ContentType::DLC, QT_TR_NOOP("DLC"), QT_TR_NOOP("DLCs"), "dlc"},
        {Core::ContentType::Savegame, QT_TR_NOOP("Save Data"), QT_TR_NOOP("Save Data"), "save_data"},
        {Core::ContentType::Extdata, QT_TR_NOOP("Extra Data"), QT_TR_NOOP("Extra Data"), "save_data"},
        {Core::ContentType::SystemArchive, QT_TR_NOOP("System Archive"), QT_TR_NOOP("System Archives"), "system_archive"},
        {Core::ContentType::Sysdata, QT_TR_NOOP("System Data"), QT_TR_NOOP("System Data"), "system_data"},
        {Core::ContentType::SystemTitle, QT_TR_NOOP("System Title"), QT_TR_NOOP("System Titles"), "hos"},
        {Core::ContentType::SystemApplet, QT_TR_NOOP("System Applet"), QT_TR_NOOP("System Applets"), "hos"},
    }};
// clang-format on

static QString GetContentName(const Core::ContentSpecifier& specifier) {
    return specifier.name.empty()
               ? QStringLiteral("0x%1").arg(specifier.id, 16, 16, QLatin1Char('0'))
               : QString::fromStdString(specifier.name);
}

template <bool Plural = true>
static QString GetContentTypeName(Core::ContentType type) {
    if constexpr (Plural) {
        return QObject::tr(std::get<2>(ContentTypeMap.at(static_cast<std::size_t>(type))),
                           "ImportDialog");
    } else {
        return QObject::tr(std::get<1>(ContentTypeMap.at(static_cast<std::size_t>(type))),
                           "ImportDialog");
    }
}

static QPixmap GetContentTypeIcon(Core::ContentType type) {
    return QIcon::fromTheme(
               QString::fromUtf8(std::get<3>(ContentTypeMap.at(static_cast<std::size_t>(type)))))
        .pixmap(24);
}

static QPixmap GetContentIcon(const Core::ContentSpecifier& specifier,
                              bool use_category_icon = false) {
    if (specifier.icon.empty()) {
        // Return a category icon, or a null icon
        return use_category_icon ? GetContentTypeIcon(specifier.type)
                                 : QIcon::fromTheme(QStringLiteral("unknown")).pixmap(24);
    }
    return QPixmap::fromImage(QImage(reinterpret_cast<const uchar*>(specifier.icon.data()), 24, 24,
                                     QImage::Format::Format_RGB16));
}

ImportDialog::ImportDialog(QWidget* parent, const Core::Config& config_)
    : DPIAwareDialog(parent, 560, 320), ui(std::make_unique<Ui::ImportDialog>()), config(config_) {

    qRegisterMetaType<u64>("u64");
    qRegisterMetaType<std::size_t>("std::size_t");
    qRegisterMetaType<Core::ContentSpecifier>();

    ui->setupUi(this);
    setWindowFlags(windowFlags() & (~Qt::WindowContextHelpButtonHint));

    RelistContent();
    UpdateSizeDisplay();

    ui->title_view_button->setChecked(true);

    ui->buttonBox->button(QDialogButtonBox::StandardButton::Reset)->setText(tr("Refresh"));
    connect(ui->buttonBox, &QDialogButtonBox::clicked, [this](QAbstractButton* button) {
        if (button == ui->buttonBox->button(QDialogButtonBox::StandardButton::Ok)) {
            StartImporting();
        } else if (button == ui->buttonBox->button(QDialogButtonBox::StandardButton::Cancel)) {
            reject();
        } else {
            RelistContent();
        }
    });

    connect(ui->title_view_button, &QRadioButton::toggled, this, &ImportDialog::RepopulateContent);
    connect(ui->advanced_button, &QPushButton::clicked, this, &ImportDialog::ShowAdvancedMenu);

    ui->main->sortByColumn(-1, Qt::AscendingOrder); // disable sorting by default
    ui->main->header()->setStretchLastSection(false);
    connect(ui->main, &QTreeWidget::customContextMenuRequested, this, &ImportDialog::OnContextMenu);
}

ImportDialog::~ImportDialog() = default;

void ImportDialog::SetContentSizes(int previous_width, int previous_height) {
    const int current_width = width();
    if (previous_width == 0) { // first time
        ui->main->setColumnWidth(0, current_width * 0.66);
        ui->main->setColumnWidth(1, current_width * 0.145);
        ui->main->setColumnWidth(2, current_width * 0.09);
    } else { // proportionally update column widths
        for (int i : {0, 1, 2}) {
            ui->main->setColumnWidth(i, ui->main->columnWidth(i) * current_width / previous_width);
        }
    }
}

void ImportDialog::RelistContent() {
    auto* dialog = new QProgressDialog(tr("Loading Contents..."), tr("Cancel"), 0, 0, this);
    dialog->setWindowFlags(dialog->windowFlags() & (~Qt::WindowContextHelpButtonHint));
    dialog->setWindowModality(Qt::WindowModal);
    dialog->setCancelButton(nullptr);
    dialog->setMinimumDuration(0);
    dialog->setValue(0);

    using FutureWatcher = QFutureWatcher<void>;
    auto* future_watcher = new FutureWatcher(this);
    connect(future_watcher, &FutureWatcher::finished, this, [this, dialog] {
        dialog->hide();
        if (importer->IsGood()) {
            RepopulateContent();
        } else {
            QMessageBox::critical(
                this, tr("Importer Error"),
                tr("Failed to initalize the importer.\nRefer to the log for details."));
            reject();
        }
    });

    auto future = QtConcurrent::run(
        [&importer = this->importer, &config = this->config, &contents = this->contents] {
            if (!importer) {
                importer = std::make_unique<Core::SDMCImporter>(config);
            }
            if (importer->IsGood()) {
                contents = importer->ListContent();
            }
        });
    future_watcher->setFuture(future);
}

constexpr Qt::ItemDataRole SpecifierIndexRole = Qt::UserRole;

/// Supports readable size display and sorting
class ContentListItem final : public QTreeWidgetItem {
public:
    explicit ContentListItem(QString name, u64 content_size_, QString exists)
        : QTreeWidgetItem{{std::move(name), ReadableByteSize(content_size_), std::move(exists)}},
          content_size(content_size_) {}

    explicit ContentListItem(QString name, u64 content_size_, QString exists, std::size_t idx)
        : QTreeWidgetItem{{std::move(name), ReadableByteSize(content_size_), std::move(exists)}},
          content_size(content_size_) {
        setData(0, SpecifierIndexRole, static_cast<int>(idx));
    }
    ~ContentListItem() override = default;

private:
    bool operator<(const QTreeWidgetItem& other_item) const override {
        const auto* other = dynamic_cast<const ContentListItem*>(&other_item);
        if (!other) {
            return false;
        }

        const int column = treeWidget()->sortColumn();
        if (column == 1) { // size
            return content_size < other->content_size;
        } else {
            return text(column) < other->text(column);
        }
    }

    u64 content_size;
};

void ImportDialog::InsertTopLevelItem(QString text, QPixmap icon) {
    auto* item = new QTreeWidgetItem{{text}};
    item->setIcon(0, QIcon(std::move(icon)));

    item->setFlags(item->flags() | Qt::ItemIsAutoTristate);
    item->setCheckState(0, Qt::Unchecked); // required to give the item a checkbox

    ui->main->invisibleRootItem()->addChild(item);
    item->setFirstColumnSpanned(true);
}

void ImportDialog::InsertTopLevelItem(QString text, QPixmap icon, u64 total_size, QString exists) {
    auto* item = new ContentListItem{std::move(text), total_size, std::move(exists)};
    item->setIcon(0, QIcon(std::move(icon)));

    item->setFlags(item->flags() | Qt::ItemIsAutoTristate);
    item->setCheckState(0, Qt::Unchecked); // required to give the item a checkbox

    ui->main->invisibleRootItem()->addChild(item);
}

// Content types that themselves form a 'Title' like entity.
constexpr std::array<Core::ContentType, 4> SpecialContentTypeList{{
    Core::ContentType::SystemArchive,
    Core::ContentType::Sysdata,
    Core::ContentType::SystemTitle,
    Core::ContentType::SystemApplet,
}};

void ImportDialog::InsertSecondLevelItem(std::size_t row, const Core::ContentSpecifier& content,
                                         std::size_t id, QString replace_name,
                                         QPixmap replace_icon) {
    const bool use_title_view = ui->title_view_button->isChecked();

    QString name;
    if (use_title_view) {
        if (row == 0) {
            name = QStringLiteral("%1 (%2)")
                       .arg(GetContentName(content))
                       .arg(GetContentTypeName<false>(content.type));
        } else if (row <= SpecialContentTypeList.size()) {
            name = GetContentName(content);
        } else {
            name = GetContentTypeName<false>(content.type);
        }
    } else {
        name = GetContentName(content);
    }

    if (!replace_name.isEmpty()) {
        name = replace_name;
    }

    auto* item = new ContentListItem{name, content.maximum_size,
                                     content.already_exists ? tr("Yes") : tr("No"), id};

    // Set icon
    QPixmap icon;
    if (replace_icon.isNull()) {
        // Exclude system titles, they are a single group but have own icons.
        if (use_title_view && content.type != Core::ContentType::SystemTitle &&
            content.type != Core::ContentType::SystemApplet) {
            icon = GetContentTypeIcon(content.type);
        } else {
            // When not in title view, System Data and System Archive groups use category icons.
            const bool use_category_icon = content.type == Core::ContentType::Sysdata ||
                                           content.type == Core::ContentType::SystemArchive;
            icon = GetContentIcon(content, use_category_icon);
        }
    } else {
        icon = replace_icon;
    }
    item->setIcon(0, QIcon(icon));

    // Skip System Applets, but enable everything else by default.
    if (!content.already_exists && content.type != Core::ContentType::SystemApplet) {
        item->setCheckState(0, Qt::Checked);
        total_selected_size += content.maximum_size;
    } else {
        item->setCheckState(0, Qt::Unchecked);
    }

    ui->main->invisibleRootItem()->child(row)->addChild(item);
}

void ImportDialog::OnItemChanged(QTreeWidgetItem* item, int column) {
    // Only handle second level items (with checkboxes)
    if (column != 0 || !item->parent()) {
        return;
    }

    const auto& specifier = SpecifierFromItem(item);
    if (item->checkState(0) == Qt::Checked) {
        if (!applet_warning_shown && !specifier.already_exists &&
            specifier.type == Core::ContentType::SystemApplet) {

            QMessageBox::warning(
                this, tr("Warning"),
                tr("You are trying to import System Applets.\nThese are known to cause problems "
                   "with certain games.\nOnly proceed if you understand what you are doing."));
            applet_warning_shown = true;
        }
        total_selected_size += specifier.maximum_size;
    } else {
        if (!system_warning_shown && !specifier.already_exists &&
            (specifier.type == Core::ContentType::SystemArchive ||
             specifier.type == Core::ContentType::Sysdata ||
             specifier.type == Core::ContentType::SystemTitle)) {

            QMessageBox::warning(this, tr("Warning"),
                                 tr("You are de-selecting important files that may be necessary "
                                    "for your imported games to run.\nIt is highly recommended to "
                                    "import these contents if they do not exist yet."));
            system_warning_shown = true;
        }
        total_selected_size -= specifier.maximum_size;
    }
    UpdateSizeDisplay();
}

void ImportDialog::RepopulateContent() {
    if (contents.empty()) { // why???
        QMessageBox::warning(this, tr("threeSD"), tr("Sorry, there are no contents available."));
        reject();
        return;
    }

    total_selected_size = 0;
    ui->main->clear();
    ui->main->setSortingEnabled(false);
    disconnect(ui->main, &QTreeWidget::itemChanged, this, &ImportDialog::OnItemChanged);

    struct TitleMapEntry {
        QString name;
        QPixmap icon;
        std::vector<const Core::ContentSpecifier*> contents;
    };
    std::map<u64, TitleMapEntry> title_map;
    std::unordered_map<u64, u64> extdata_id_map; // extdata ID -> title ID
    for (const auto& content : contents) {
        if (content.type == Core::ContentType::Application) {
            title_map[content.id].name = GetContentName(content);
            title_map[content.id].icon = GetContentIcon(content);
            extdata_id_map.emplace(content.extdata_id, content.id);
        }
    }
    for (const auto& content : contents) {
        if (content.type == Core::ContentType::Extdata) {
            if (extdata_id_map.count(content.id)) {
                const u64 title_id = extdata_id_map.at(content.id);
                title_map[title_id].contents.emplace_back(&content);
            }
        } else if (content.type == Core::ContentType::Application ||
                   content.type == Core::ContentType::Update ||
                   content.type == Core::ContentType::DLC ||
                   content.type == Core::ContentType::Savegame) {
            if (title_map.count(content.id)) {
                title_map[content.id].contents.emplace_back(&content);
            }
        }
    }

    const bool use_title_view = ui->title_view_button->isChecked();
    if (use_title_view) {
        // Create 'Ungrouped' category.
        InsertTopLevelItem(tr("Ungrouped"), QIcon::fromTheme(QStringLiteral("unknown")).pixmap(24));

        // Create categories for special content types.
        for (std::size_t i = 0; i < SpecialContentTypeList.size(); ++i) {
            InsertTopLevelItem(GetContentTypeName(SpecialContentTypeList[i]),
                               GetContentTypeIcon(SpecialContentTypeList[i]));
        }

        // Titles
        std::unordered_map<u64, u64> title_row_map;
        for (auto& [id, entry] : title_map) {
            // Process the title's contents
            u64 total_size = 0;
            bool has_exist = false, has_non_exist = false;
            for (const auto* content : entry.contents) {
                total_size += content->maximum_size;
                if (content->already_exists) {
                    has_exist = true;
                } else {
                    has_non_exist = true;
                }
            }

            QString exist_text;
            if (!has_exist) {
                exist_text = tr("No");
            } else if (!has_non_exist) {
                exist_text = tr("Yes");
            } else {
                exist_text = tr("Part");
            }

            InsertTopLevelItem(std::move(entry.name), std::move(entry.icon), total_size,
                               std::move(exist_text));
            title_row_map.emplace(id, ui->main->invisibleRootItem()->childCount() - 1);
        }

        for (std::size_t i = 0; i < contents.size(); ++i) {
            const auto& content = contents[i];

            std::size_t row = 0; // 0 for ungrouped (default)
            switch (content.type) {
            case Core::ContentType::Application:
            case Core::ContentType::Update:
            case Core::ContentType::DLC:
            case Core::ContentType::Savegame: {
                // Fix the id
                const auto real_id = content.id & 0xffffff00ffffffff;
                row = title_row_map.count(real_id) ? title_row_map.at(real_id) : 0;
                break;
            }
            case Core::ContentType::Extdata: {
                if (extdata_id_map.count(content.id)) {
                    row = title_row_map.at(extdata_id_map.at(content.id));
                } else {
                    row = 0; // Ungrouped
                }
                break;
            }
            default: {
                const std::size_t idx = std::find(SpecialContentTypeList.begin(),
                                                  SpecialContentTypeList.end(), content.type) -
                                        SpecialContentTypeList.begin();
                ASSERT_MSG(idx < SpecialContentTypeList.size(), "Content Type not handled");
                row = idx + 1;
                break;
            }
            }

            InsertSecondLevelItem(row, content, i);
        }
    } else {
        for (const auto& [type, singular_name, plural_name, icon_name] : ContentTypeMap) {
            InsertTopLevelItem(tr(plural_name), GetContentTypeIcon(type));
        }

        for (std::size_t i = 0; i < contents.size(); ++i) {
            const auto& content = contents[i];

            QString name{};
            QPixmap icon{};
            if (content.type == Core::ContentType::Savegame) {
                if (title_map.count(content.id)) {
                    name = title_map.at(content.id).name;
                    icon = title_map.at(content.id).icon;
                }
            } else if (content.type == Core::ContentType::Extdata) {
                if (extdata_id_map.count(content.id)) {
                    u64 title_id = extdata_id_map.at(content.id);
                    name = title_map.at(title_id).name;
                    icon = title_map.at(title_id).icon;
                }
            }

            InsertSecondLevelItem(static_cast<std::size_t>(content.type), content, i, name, icon);
        }
    }

    ui->main->setSortingEnabled(true);
    connect(ui->main, &QTreeWidget::itemChanged, this, &ImportDialog::OnItemChanged);
    UpdateSizeDisplay();
}

void ImportDialog::UpdateSizeDisplay() {
    QStorageInfo storage(QString::fromStdString(config.user_path));
    if (!storage.isValid() || !storage.isReady()) {
        LOG_ERROR(Frontend, "Storage {} is not good", config.user_path);
        QMessageBox::critical(
            this, tr("Bad Storage"),
            tr("An error occured while trying to get available space for the storage."));
        reject();
        return;
    }

    ui->availableSpace->setText(
        tr("Available Space: %1").arg(ReadableByteSize(storage.bytesAvailable())));
    ui->totalSize->setText(tr("Total Size: %1").arg(ReadableByteSize(total_selected_size)));

    ui->buttonBox->button(QDialogButtonBox::StandardButton::Ok)
        ->setEnabled(total_selected_size > 0 &&
                     total_selected_size <= static_cast<u64>(storage.bytesAvailable()));
}

std::vector<Core::ContentSpecifier> ImportDialog::GetSelectedContentList() {
    std::vector<Core::ContentSpecifier> to_import;
    for (int i = 0; i < ui->main->invisibleRootItem()->childCount(); ++i) {
        const auto* item = ui->main->invisibleRootItem()->child(i);
        for (int j = 0; j < item->childCount(); ++j) {
            if (item->child(j)->checkState(0) == Qt::Checked) {
                to_import.emplace_back(
                    contents[item->child(j)->data(0, SpecifierIndexRole).toInt()]);
            }
        }
    }

    return to_import;
}

Core::ContentSpecifier ImportDialog::SpecifierFromItem(QTreeWidgetItem* item) const {
    return contents[item->data(0, SpecifierIndexRole).toInt()];
}

void ImportDialog::OnContextMenu(const QPoint& point) {
    QTreeWidgetItem* item = ui->main->itemAt(point.x(), point.y());
    if (!item) {
        return;
    }

    const bool title_view = ui->title_view_button->isChecked();

    QMenu context_menu;
    if (item->parent()) { // Second level
        const auto& specifier = SpecifierFromItem(item);
        if (specifier.type == Core::ContentType::Application) {
            context_menu.addAction(tr("Dump CXI file"),
                                   [this, specifier] { StartDumpingCXISingle(specifier); });
        }
        if (Core::IsTitle(specifier.type)) {
            context_menu.addAction(tr("Build CIA..."),
                                   [this, specifier] { StartBuildingCIASingle(specifier); });
            context_menu.addAction(tr("Show Title Info"), [this, specifier] {
                TitleInfoDialog dialog(this, *importer, specifier);
                dialog.exec();
            });
        }
    } else { // Top level
        if (!title_view) {
            return;
        }

        for (int i = 0; i < item->childCount(); ++i) {
            const auto& specifier = SpecifierFromItem(item->child(i));
            if (specifier.type == Core::ContentType::Application) {
                context_menu.addAction(tr("Dump Base CXI file"),
                                       [this, specifier] { StartDumpingCXISingle(specifier); });
                context_menu.addAction(tr("Build Base CIA"),
                                       [this, specifier] { StartBuildingCIASingle(specifier); });
            } else if (specifier.type == Core::ContentType::Update) {
                context_menu.addAction(tr("Build Update CIA"),
                                       [this, specifier] { StartBuildingCIASingle(specifier); });
            } else if (specifier.type == Core::ContentType::DLC) {
                context_menu.addAction(tr("Build DLC CIA"),
                                       [this, specifier] { StartBuildingCIASingle(specifier); });
            }
        }
    }
    context_menu.exec(ui->main->viewport()->mapToGlobal(point));
}

class AdvancedMenu : public QMenu {
public:
    explicit AdvancedMenu(QWidget* parent) : QMenu(parent) {}

private:
    void mousePressEvent(QMouseEvent* event) override {
        auto* dialog = static_cast<ImportDialog*>(parentWidget());
        // Block popup menu when clicking on the Advanced button to dismiss the menu.
        // With out this, it will immediately bring up the menu again.
        if (dialog->childAt(dialog->mapFromGlobal(event->globalPos())) ==
            dialog->ui->advanced_button) {

            dialog->block_advanced_menu = true;
        }

        QMenu::mousePressEvent(event);
    }
};

void ImportDialog::ShowAdvancedMenu() {
    if (block_advanced_menu) {
        block_advanced_menu = false;
        return;
    }

    AdvancedMenu menu(this);
    menu.addAction(tr("Batch Dump CXI"), this, &ImportDialog::StartBatchDumpingCXI);
    menu.addAction(tr("Batch Build CIA"), this, &ImportDialog::StartBatchBuildingCIA);

    menu.exec(ui->advanced_button->mapToGlobal(ui->advanced_button->rect().bottomLeft()));
}

static QString FormatETA(int eta) {
    if (eta < 0) {
        return QStringLiteral("&nbsp;");
    }
    return QCoreApplication::translate("ImportDialog", "ETA %1m%2s")
        .arg(eta / 60, 2, 10, QLatin1Char('0'))
        .arg(eta % 60, 2, 10, QLatin1Char('0'));
}

// Runs the job, opening a dialog to report is progress.
void ImportDialog::RunMultiJob(MultiJob* job, std::size_t total_count, u64 total_size) {
    // Try to map total_size to int range
    // This is equal to ceil(total_size / INT_MAX)
    const u64 multiplier =
        (total_size + std::numeric_limits<int>::max() - 1) / std::numeric_limits<int>::max();

    auto* label = new QLabel(tr("Initializing..."));
    label->setWordWrap(true);
    label->setFixedWidth(600);

    // We need to create the bar ourselves to circumvent an issue caused by modal ProgressDialog's
    // event handling.
    auto* bar = new QProgressBar(this);
    bar->setRange(0, static_cast<int>(total_size / multiplier));
    bar->setValue(0);

    auto* dialog = new QProgressDialog(tr("Initializing..."), tr("Cancel"), 0, 0, this);
    dialog->setWindowFlags(dialog->windowFlags() & (~Qt::WindowContextHelpButtonHint));
    dialog->setWindowModality(Qt::WindowModal);
    dialog->setBar(bar);
    dialog->setLabel(label);
    dialog->setMinimumDuration(0);

    connect(job, &MultiJob::NextContent, this,
            [this, dialog, total_count](std::size_t count,
                                        const Core::ContentSpecifier& next_content, int eta) {
                dialog->setLabelText(
                    tr("<p>(%1/%2) %3 (%4)</p><p>&nbsp;</p><p align=\"right\">%5</p>")
                        .arg(count)
                        .arg(total_count)
                        .arg(GetContentName(next_content))
                        .arg(GetContentTypeName<false>(next_content.type))
                        .arg(FormatETA(eta)));
                current_content = next_content;
                current_count = count;
            });
    connect(job, &MultiJob::ProgressUpdated, this,
            [this, bar, dialog, multiplier, total_count](u64 current_imported_size,
                                                         u64 total_imported_size, int eta) {
                bar->setValue(static_cast<int>(total_imported_size / multiplier));
                dialog->setLabelText(tr("<p>(%1/%2) %3 (%4)</p><p align=\"center\">%5 "
                                        "/ %6</p><p align=\"right\">%7</p>")
                                         .arg(current_count)
                                         .arg(total_count)
                                         .arg(GetContentName(current_content))
                                         .arg(GetContentTypeName<false>(current_content.type))
                                         .arg(ReadableByteSize(current_imported_size))
                                         .arg(ReadableByteSize(current_content.maximum_size))
                                         .arg(FormatETA(eta)));
            });
    connect(job, &MultiJob::Completed, this, [this, dialog, job] {
        dialog->setValue(dialog->maximum());

        const auto failed_contents = job->GetFailedContents();
        if (failed_contents.empty()) {
            QMessageBox::information(this, tr("threeSD"), tr("All contents done successfully."));
        } else {
            QString list_content;
            for (const auto& content : failed_contents) {
                list_content.append(QStringLiteral("<li>%1 (%2)</li>")
                                        .arg(GetContentName(content))
                                        .arg(GetContentTypeName<false>(content.type)));
            }
            QMessageBox::critical(this, tr("threeSD"),
                                  tr("List of failed contents:<ul>%1</ul>").arg(list_content));
        }

        RelistContent();
    });
    connect(dialog, &QProgressDialog::canceled, this, [this, job] {
        // Add yet-another-ProgressDialog to indicate cancel progress
        auto* cancel_dialog = new QProgressDialog(tr("Canceling..."), tr("Cancel"), 0, 0, this);
        cancel_dialog->setWindowFlags(cancel_dialog->windowFlags() &
                                      (~Qt::WindowContextHelpButtonHint));
        cancel_dialog->setWindowModality(Qt::WindowModal);
        cancel_dialog->setCancelButton(nullptr);
        cancel_dialog->setMinimumDuration(0);
        cancel_dialog->setValue(0);
        connect(job, &MultiJob::Completed, cancel_dialog, &QProgressDialog::hide);
        job->Cancel();
    });

    job->start();
}

void ImportDialog::StartImporting() {
    UpdateSizeDisplay();
    if (!ui->buttonBox->button(QDialogButtonBox::StandardButton::Ok)->isEnabled()) {
        // Space is no longer enough
        QMessageBox::warning(this, tr("Not Enough Space"),
                             tr("Your disk does not have enough space to hold imported data."));
        return;
    }

    auto to_import = GetSelectedContentList();
    const std::size_t total_count = to_import.size();

    auto* job =
        new MultiJob(this, *importer, std::move(to_import), &Core::SDMCImporter::ImportContent,
                     &Core::SDMCImporter::AbortImporting);

    RunMultiJob(job, total_count, total_selected_size);
}

// CXI dumping

void ImportDialog::StartDumpingCXISingle(const Core::ContentSpecifier& specifier) {
    const QString path = QFileDialog::getSaveFileName(this, tr("Dump CXI file"), last_dump_cxi_path,
                                                      tr("CTR Executable Image (*.cxi)"));
    if (path.isEmpty()) {
        return;
    }
    last_dump_cxi_path = QFileInfo(path).path();

    auto* job = new SimpleJob(
        this,
        [this, specifier, path](const Common::ProgressCallback& callback) {
            return importer->DumpCXI(specifier, path.toStdString(), callback);
        },
        [this] { importer->AbortDumpCXI(); });
    job->StartWithProgressDialog(this);
}

void ImportDialog::StartBatchDumpingCXI() {
    auto to_import = GetSelectedContentList();
    if (to_import.empty()) {
        QMessageBox::warning(this, tr("threeSD"),
                             tr("Please select the contents you would like to dump as CXIs."));
        return;
    }

    const auto removed_iter = std::remove_if(
        to_import.begin(), to_import.end(), [](const Core::ContentSpecifier& specifier) {
            return specifier.type != Core::ContentType::Application;
        });
    if (removed_iter == to_import.begin()) { // No Applications selected
        QMessageBox::critical(this, tr("threeSD"),
                              tr("The contents selected are not supported.<br>You can only dump "
                                 "Applications as CXIs."));
        return;
    }
    if (removed_iter != to_import.end()) { // Some non-Applications selected
        QMessageBox::warning(this, tr("threeSD"),
                             tr("Some contents selected are not supported and will be "
                                "ignored.<br>Only Applications will be dumped as CXIs."));
    }

    to_import.erase(removed_iter, to_import.end());

    QString path =
        QFileDialog::getExistingDirectory(this, tr("Batch Dump CXI"), last_batch_dump_cxi_path);
    if (path.isEmpty()) {
        return;
    }
    last_batch_dump_cxi_path = path;
    if (!path.endsWith(QChar::fromLatin1('/')) && !path.endsWith(QChar::fromLatin1('\\'))) {
        path.append(QStringLiteral("/"));
    }

    const auto total_count = to_import.size();
    const auto total_size = std::accumulate(to_import.begin(), to_import.end(), u64{0},
                                            [](u64 sum, const Core::ContentSpecifier& specifier) {
                                                return sum + specifier.maximum_size;
                                            });
    auto* job = new MultiJob(
        this, *importer, std::move(to_import),
        [path](Core::SDMCImporter& importer, const Core::ContentSpecifier& specifier,
               const Common::ProgressCallback& callback) {
            return importer.DumpCXI(specifier, path.toStdString(), callback, true);
        },
        &Core::SDMCImporter::AbortDumpCXI);
    RunMultiJob(job, total_count, total_size);
}

// CIA building

void ImportDialog::StartBuildingCIASingle(const Core::ContentSpecifier& specifier) {
    CIABuildDialog dialog(this,
                          /*is_dir*/ false,
                          /*is_nand*/ Core::IsNandTitle(specifier.type),
                          /*enable_legit*/ importer->CanBuildLegitCIA(specifier),
                          last_build_cia_path);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const auto& [path, type] = dialog.GetResults();
    last_build_cia_path = QFileInfo(path).path();

    auto* job = new SimpleJob(
        this,
        [this, specifier, path = path, type = type](const Common::ProgressCallback& callback) {
            return importer->BuildCIA(type, specifier, path.toStdString(), callback);
        },
        [this] { importer->AbortBuildCIA(); });
    job->StartWithProgressDialog(this);
}

void ImportDialog::StartBatchBuildingCIA() {
    auto to_import = GetSelectedContentList();
    if (to_import.empty()) {
        QMessageBox::warning(this, tr("threeSD"),
                             tr("Please select the contents you would like to build as CIAs."));
        return;
    }

    const auto removed_iter = std::remove_if(
        to_import.begin(), to_import.end(),
        [](const Core::ContentSpecifier& specifier) { return !Core::IsTitle(specifier.type); });
    if (removed_iter == to_import.begin()) { // No Titles selected
        QMessageBox::critical(this, tr("threeSD"),
                              tr("The contents selected are not supported.<br>You can only build "
                                 "CIAs from Applications, Updates, DLCs and System Titles."));
        return;
    }
    if (removed_iter != to_import.end()) { // Some non-Titles selected
        QMessageBox::warning(
            this, tr("threeSD"),
            tr("Some contents selected are not supported and will be ignored.<br>Only "
               "Applications, Updates, DLCs and System Titles will be built as CIAs."));
    }

    to_import.erase(removed_iter, to_import.end());

    const bool is_nand = std::all_of(
        to_import.begin(), to_import.end(),
        [](const Core::ContentSpecifier& specifier) { return Core::IsNandTitle(specifier.type); });
    const bool enable_legit = std::all_of(to_import.begin(), to_import.end(),
                                          [this](const Core::ContentSpecifier& specifier) {
                                              return importer->CanBuildLegitCIA(specifier);
                                          });
    CIABuildDialog dialog(this, /*is_dir*/ true, is_nand, enable_legit, last_batch_build_cia_path);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    auto [path, type] = dialog.GetResults();
    last_batch_build_cia_path = path;
    if (!path.endsWith(QChar::fromLatin1('/')) && !path.endsWith(QChar::fromLatin1('\\'))) {
        path.append(QStringLiteral("/"));
    }

    const auto total_count = to_import.size();
    const auto total_size = std::accumulate(to_import.begin(), to_import.end(), u64{0},
                                            [](u64 sum, const Core::ContentSpecifier& specifier) {
                                                return sum + specifier.maximum_size;
                                            });
    auto* job = new MultiJob(
        this, *importer, std::move(to_import),
        [path = path, type = type](Core::SDMCImporter& importer,
                                   const Core::ContentSpecifier& specifier,
                                   const Common::ProgressCallback& callback) {
            return importer.BuildCIA(type, specifier, path.toStdString(), callback, true);
        },
        &Core::SDMCImporter::AbortBuildCIA);
    RunMultiJob(job, total_count, total_size);
}
