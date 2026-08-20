#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMetaObject>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
#include <QSplitter>
#include <QSet>
#include <QMap>
#include <QStringList>
#include <QShowEvent>
#include <QSpinBox>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <thread>
#include <vector>

extern "C" {
#include "app_controller.h"
#include "cat.h"
#include "config.h"
#include "cty.h"
#include "scp.h"
#include "cw_keys.h"
#include "db.h"
#include "dxcluster.h"
#include "globals.h"
#include "net_sync.h"
#include "qso.h"
#include "qtc.h"
#include "stats.h"
}

namespace {
constexpr std::array<const char *, 11> kBandLabels = {
    "160M", "80M", "40M", "30M", "20M", "17M", "15M", "12M",
    "10M", "6M", "2M"};

/*
 * Return the band index for a known band label.
 *
 * @param band Band label to search for.
 * @return Zero-based index, or -1 if the band is unknown.
 */
int band_index(const char *band) {
  if (!band)
    return -1;

  for (int i = 0; i < (int)kBandLabels.size(); i++) {
    if (std::strcmp(band, kBandLabels[i]) == 0)
      return i;
  }

  return -1;
}

/*
 * Parse a spot frequency string into rounded kilohertz.
 *
 * @param freq_text Text representation of the frequency.
 * @return Rounded kilohertz value, or 0 if the text cannot be parsed.
 */
int parse_spot_khz(const char *freq_text) {
  if (!freq_text || !freq_text[0])
    return 0;

  char *end = nullptr;
  const double value = std::strtod(freq_text, &end);
  if (end == freq_text)
    return 0;

  return (int)std::lround(value);
}

QString render_rst_sr(const QSO &q) {
  const QString mode = QString::fromLatin1(q.mode);

  QString rst_recv = QString::fromLatin1(q.rst);
  if (rst_recv.isEmpty())
    rst_recv = "59";

  const QString rst_sent = (mode == "CW") ? "599" : "59";

  return QString("%1/%2").arg(rst_sent, rst_recv);
}

QString render_exchange_sr(const QSO &q) {
  const QString sent = QString::fromLatin1(q.exchange_sent);
  const QString recv = QString::fromLatin1(q.exchange_recv);

  if (sent.isEmpty() && recv.isEmpty())
    return render_rst_sr(q);

  return QString("%1/%2").arg(sent, recv);
}

int technique_combo_index(ContestTechnique technique) {
  switch (technique) {
  case CONTEST_TECH_SO2V:
    return 1;
  case CONTEST_TECH_SO2R:
    return 2;
  case CONTEST_TECH_SO1R:
  default:
    return 0;
  }
}

ContestTechnique technique_from_combo_index(int index) {
  switch (index) {
  case 1:
    return CONTEST_TECH_SO2V;
  case 2:
    return CONTEST_TECH_SO2R;
  case 0:
  default:
    return CONTEST_TECH_SO1R;
  }
}

struct ContestPreset {
  const char *label;
  const char *path;
};

constexpr std::array<ContestPreset, 5> kContestPresets = {{
    {"IARU HF Championship", "contest_defs/iaru_hf_championship.conf"},
    {"CQ WW CW", "contest_defs/cq_ww_cw.conf"},
    {"CQ WW SSB", "contest_defs/cq_ww_ssb.conf"},
    {"CQ WPX SSB", "contest_defs/cq_wpx_ssb.conf"},
    {"CQ WPX CW", "contest_defs/cq_wpx_cw.conf"},
}};

/*
 * Scan the contest_defs/ directory (relative to executable or CWD) and
 * return a sorted list of {display_label, relative_path} pairs for every
 * .conf file found.  The display label is read from the NAME= key inside
 * the file; if the file cannot be parsed the base filename is used.
 *
 * Searches the following roots in order, stopping at the first one that
 * contains at least one .conf file:
 *   1. <CWD>/contest_defs/
 *   2. <app_dir>/contest_defs/
 *   3. <app_dir>/../contest_defs/
 *   4. <app_dir>/../../contest_defs/
 */
static std::vector<std::pair<QString, QString>> discover_contest_presets() {
  /* Search for contest_defs/ relative to the binary location only.
   * CWD is intentionally excluded so the list is always consistent
   * regardless of where the application was launched from. */
  const QString app_dir = QApplication::applicationDirPath();
  const QStringList search_roots = {
      app_dir,
      app_dir + "/..",
      app_dir + "/../..",
  };

  QDir contest_dir;
  for (const QString &root : search_roots) {
    QDir candidate(root + "/contest_defs");
    if (candidate.exists() && !candidate.entryList({"*.conf"}, QDir::Files).isEmpty()) {
      contest_dir = candidate;
      break;
    }
  }

  if (!contest_dir.exists())
    return {};

  const QStringList files = contest_dir.entryList({"*.conf"}, QDir::Files, QDir::Name);

  std::vector<std::pair<QString, QString>> result;
  result.reserve((size_t)files.size());

  for (const QString &filename : files) {
    /* Use the canonical absolute path so app_controller's path resolver
     * can open the file with access(path, R_OK) on the first try,
     * regardless of the process working directory. */
    const QString abs_path = QFileInfo(contest_dir.filePath(filename)).canonicalFilePath();
    const QByteArray abs_bytes = abs_path.toLocal8Bit();

    ContestDefinition def;
    char err[64] = {0};
    QString label;
    if (contest_definition_load(abs_bytes.constData(), &def, err, sizeof(err)) == 0
        && def.name[0]) {
      label = QString::fromLatin1(def.name);
    } else {
      label = QFileInfo(filename).completeBaseName();
    }

    result.emplace_back(label, abs_path);
  }

  /* Sort alphabetically by display label (case-insensitive). */
  std::sort(result.begin(), result.end(),
            [](const std::pair<QString, QString> &a,
               const std::pair<QString, QString> &b) {
              return a.first.compare(b.first, Qt::CaseInsensitive) < 0;
            });

  return result;
}

const char *multiplier_to_text_for_save(ContestMultiplierType type) {
  switch (type) {
  case CONTEST_MULT_NONE:
    return "NONE";
  case CONTEST_MULT_DXCC:
    return "DXCC";
  case CONTEST_MULT_DXCC_PER_BAND:
    return "DXCC_PER_BAND";
  case CONTEST_MULT_ZONE_PER_BAND:
    return "ZONE_PER_BAND";
  case CONTEST_MULT_ZONE:
    return "ZONE";
  case CONTEST_MULT_PREFIX:
    return "PREFIX";
  case CONTEST_MULT_PREFIX_PER_BAND:
    return "PREFIX_PER_BAND";
  case CONTEST_MULT_DXCC_PLUS_ZONE_PER_BAND:
    return "DXCC_PLUS_ZONE_PER_BAND";
  case CONTEST_MULT_SPDX:
    return "SPDX";
  case CONTEST_MULT_MODE_DXCC:
    return "MODE_DXCC";
  default:
    return "DXCC";
  }
}

int save_contest_definition_file(const char *path, const ContestDefinition *def,
                                 char *error_text, size_t error_size) {
  if (error_text && error_size > 0)
    error_text[0] = 0;

  if (!path || !path[0] || !def) {
    if (error_text && error_size > 0)
      std::snprintf(error_text, error_size,
                    "Missing path or contest definition data");
    return -1;
  }

  FILE *f = std::fopen(path, "w");
  if (!f) {
    if (error_text && error_size > 0)
      std::snprintf(error_text, error_size,
                    "Cannot open destination contest file");
    return -1;
  }

  std::fprintf(f, "NAME=%s\n", def->name[0] ? def->name : "GENERAL");
  std::fprintf(f, "CABRILLO_NAME=%s\n",
               def->cabrillo_name[0] ? def->cabrillo_name : def->name);
  std::fprintf(f, "MODE=%s\n", def->mode[0] ? def->mode : "MIXED");
  std::fprintf(f, "CATEGORY_OPERATOR=%s\n",
               def->category_operator[0] ? def->category_operator :
                                           "SINGLE-OP");
  std::fprintf(f, "CATEGORY_BAND=%s\n",
               def->category_band[0] ? def->category_band : "ALL");
  std::fprintf(f, "CATEGORY_POWER=%s\n",
               def->category_power[0] ? def->category_power : "LOW");
  std::fprintf(f, "CATEGORY_OVERLAY=%s\n", def->category_overlay);
  std::fprintf(f, "STATION_LOCATION=%s\n", def->station_location);
  std::fprintf(f, "OPERATORS=%s\n", def->operators);
  std::fprintf(f, "EXCHANGE_SENT=%s\n",
               def->exchange_sent_template[0] ? def->exchange_sent_template :
                                                "#");
  std::fprintf(f, "POINTS_PER_QSO=%d\n",
               def->points_per_qso > 0 ? def->points_per_qso : 1);
  std::fprintf(f, "POINTS_CW=%d\n", def->points_cw);
  std::fprintf(f, "POINTS_PHONE=%d\n", def->points_phone);
  std::fprintf(f, "POINTS_DIGI=%d\n", def->points_digi);
  std::fprintf(f, "POINTS_NEW_DXCC=%d\n", def->points_new_dxcc);
  std::fprintf(f, "POINTS_SAME_DXCC=%d\n", def->points_same_dxcc);
  std::fprintf(f, "POINTS_NEW_BAND_DXCC=%d\n", def->points_new_band_dxcc);
  std::fprintf(f, "POINTS_SAME_BAND_DXCC=%d\n", def->points_same_band_dxcc);
  std::fprintf(f, "MULTIPLIER=%s\n",
               multiplier_to_text_for_save(def->multiplier_type));
  std::fprintf(f, "BONUS_POINTS=%d\n", def->bonus_points);

  /* QTC traffic fields (WAE and similar contests). */
  if (def->qtc_sender_side[0] && std::strcmp(def->qtc_sender_side, "NONE") != 0) {
    std::fprintf(f, "QTC_SENDER=%s\n", def->qtc_sender_side);
    std::fprintf(f, "POINTS_PER_QTC=%d\n",
                 def->points_per_qtc > 0 ? def->points_per_qtc : 1);
  }

  for (int i = 0; i < def->field_count; i++) {
    const ContestFieldDef *field = &def->fields[i];
    if (!field->name[0])
      continue;
    std::fprintf(f, "FIELD=%s,%s,%s\n", field->name,
                 field->label[0] ? field->label : field->name,
                 field->required ? "required" : "optional");
  }

  std::fclose(f);
  return 0;
}

void set_text_if_changed(QLabel *label, const QString &text) {
  if (label && label->text() != text)
    label->setText(text);
}

void set_text_if_changed(QLineEdit *edit, const QString &text) {
  if (edit && edit->text() != text)
    edit->setText(text);
}

void set_text_if_changed(QPushButton *button, const QString &text) {
  if (button && button->text() != text)
    button->setText(text);
}

void set_title_if_changed(QGroupBox *group, const QString &title) {
  if (group && group->title() != title)
    group->setTitle(title);
}

void set_stylesheet_if_changed(QWidget *widget, const QString &style) {
  if (widget && widget->styleSheet() != style)
    widget->setStyleSheet(style);
}

void set_visible_if_changed(QWidget *widget, bool visible) {
  if (widget && widget->isVisible() != visible)
    widget->setVisible(visible);
}

std::uint64_t hash_mix(std::uint64_t hash, std::uint64_t value) {
  hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6U) + (hash >> 2U);
  return hash;
}

std::uint64_t hash_cstr(std::uint64_t hash, const char *text) {
  if (!text)
    return hash_mix(hash, 0ULL);

  while (*text) {
    hash ^= (unsigned char)*text;
    hash *= 1099511628211ULL;
    ++text;
  }
  return hash;
}

std::uint64_t hash_qstring(std::uint64_t hash, const QString &text) {
  return hash_mix(hash, (std::uint64_t)qHash(text));
}
} // namespace

#include "qt_frontend_cw_dialogs.inc"
#include "qt_frontend_qtc_docs.inc"
#include "qt_frontend_main_window.inc"
#include "qt_frontend_main.inc"
