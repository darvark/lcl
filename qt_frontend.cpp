#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
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
#include <QMessageBox>
#include <QPushButton>
#include <QShowEvent>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

extern "C" {
#include "app_controller.h"
#include "cat.h"
#include "config.h"
#include "cw_keys.h"
#include "db.h"
#include "dxcluster.h"
#include "globals.h"
#include "qso.h"
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
} // namespace

class CwKeyerDialog : public QWidget {
public:
  explicit CwKeyerDialog(QWidget *parent = nullptr)
      : QWidget(parent, Qt::Dialog) {
    setWindowTitle("CW Keyer (Ctrl+K)");
    setAttribute(Qt::WA_DeleteOnClose, false);
    resize(420, 160);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(6);

    auto *slot_row = new QHBoxLayout();
    slot_row->addWidget(new QLabel("Rig:", this));
    slot_combo_ = new QComboBox(this);
    slot_combo_->addItem("Radio 1", CAT_SLOT_A);
    slot_combo_->addItem("Radio 2", CAT_SLOT_B);
    slot_row->addWidget(slot_combo_);
    slot_row->addStretch(1);
    layout->addLayout(slot_row);

    layout->addWidget(new QLabel("Wpisz tekst — znaki są nadawane CW na bieżąco:", this));

    text_edit_ = new QLineEdit(this);
    text_edit_->setPlaceholderText("Wpisz tutaj...");
    layout->addWidget(text_edit_);

    status_label_ = new QLabel(this);
    layout->addWidget(status_label_);

    auto *btn_row = new QHBoxLayout();
    stop_btn_ = new QPushButton("Stop TX (Esc)", this);
    auto *clear_btn = new QPushButton("Wyczyść", this);
    btn_row->addWidget(stop_btn_);
    btn_row->addWidget(clear_btn);
    layout->addLayout(btn_row);

    connect(text_edit_, &QLineEdit::textChanged, this,
            [this](const QString &text) { on_text_changed(text); });
    connect(stop_btn_, &QPushButton::clicked, this, [this]() { on_stop(); });
    connect(clear_btn, &QPushButton::clicked, this, [this]() {
      on_stop();
      last_sent_pos_ = 0;
      text_edit_->clear();
    });
  }

  void showAndFocus() {
    last_sent_pos_ = text_edit_->text().length();
    show();
    raise();
    activateWindow();
    text_edit_->setFocus();
  }

protected:
  void keyPressEvent(QKeyEvent *event) override {
    if (event->key() == Qt::Key_Escape) {
      on_stop();
      hide();
      event->accept();
      return;
    }
    QWidget::keyPressEvent(event);
  }

private:
  void on_text_changed(const QString &text) {
    const int current_len = text.length();
    if (current_len <= last_sent_pos_) {
      last_sent_pos_ = current_len;
      return;
    }

    const QString new_chars = text.mid(last_sent_pos_);
    last_sent_pos_ = current_len;

    const QByteArray bytes = new_chars.toUpper().toLatin1();

    if (cat_is_cw_keyer_connected()) {
      if (cat_cw_send(bytes.constData()) == 0) {
        status_label_->setText(QString("Keyer: wysyłanie \"%1\"").arg(new_chars));
        return;
      }
    }

    /* fall back to CAT rig send_morse */
    const int slot = slot_combo_->currentData().toInt();
    if (cat_send_morse_slot(slot, bytes.constData()) == 0)
      status_label_->setText(QString("CAT rig: wysyłanie \"%1\"").arg(new_chars));
    else
      status_label_->setText("Błąd — brak keyer i brak połączenia CAT");
  }

  void on_stop() {
    if (cat_is_cw_keyer_connected()) {
      cat_cw_stop();
    } else {
      const int slot = slot_combo_->currentData().toInt();
      cat_stop_morse_slot(slot);
    }
    status_label_->setText("TX zatrzymany");
  }

  QLineEdit *text_edit_ = nullptr;
  QComboBox *slot_combo_ = nullptr;
  QLabel *status_label_ = nullptr;
  QPushButton *stop_btn_ = nullptr;
  int last_sent_pos_ = 0;
};

class LoggerQtWindow : public QMainWindow {
public:
  /*
   * Build the main Qt window and wire up its widgets and timers.
   *
   * @return Nothing.
   */
  LoggerQtWindow() {
    setWindowTitle("Logger (Qt)");
    resize(1300, 860);
    setFocusPolicy(Qt::StrongFocus);

    QWidget *central = new QWidget(this);
    setCentralWidget(central);

    auto *root = new QHBoxLayout(central);
    root->setContentsMargins(4, 4, 4, 4);
    root->setSpacing(4);

    auto *left_column = new QWidget(central);
    auto *left_layout = new QVBoxLayout(left_column);
    left_layout->setContentsMargins(0, 0, 0, 0);
    left_layout->setSpacing(2);

    log_group_ = new QGroupBox("QSO Log", central);
    log_group_->setObjectName("logGroup");
    auto *log_layout = new QVBoxLayout(log_group_);
    log_layout->setContentsMargins(6, 24, 6, 6);
    log_layout->setSpacing(2);
    log_table_ = new QTableWidget(log_group_);
    log_table_->setColumnCount(9);
    log_table_->setHorizontalHeaderLabels(
      {"Nr", "Date", "UTC", "Call", "Freq", "Band", "Mode", "Exch S/R", "Run/Pts"});
    log_table_->verticalHeader()->setVisible(false);
    log_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    log_table_->setSelectionMode(QAbstractItemView::NoSelection);
    log_table_->setFocusPolicy(Qt::NoFocus);
    log_table_->setShowGrid(false);
    log_table_->setWordWrap(false);
    log_table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    log_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    log_table_->horizontalHeader()->setStretchLastSection(false);
    log_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    log_layout->addWidget(log_table_);
    left_layout->addWidget(log_group_);

    suggestions_frame_ = new QFrame(log_group_);
    suggestions_frame_->setFrameShape(QFrame::StyledPanel);
    suggestions_frame_->setFixedSize(320, 170);
    auto *suggest_layout = new QVBoxLayout(suggestions_frame_);
    suggest_layout->setContentsMargins(6, 6, 6, 6);
    suggest_layout->setSpacing(2);
    auto *suggest_title = new QLabel("Call Suggestions", suggestions_frame_);
    suggest_title->setObjectName("suggestTitle");
    suggestions_list_ = new QListWidget(suggestions_frame_);
    suggestions_list_->setFocusPolicy(Qt::NoFocus);
    suggestions_list_->setSelectionMode(QAbstractItemView::SingleSelection);
    suggestions_list_->setUniformItemSizes(true);
    suggestions_list_->setWordWrap(false);
    suggest_layout->addWidget(suggest_title);
    suggest_layout->addWidget(suggestions_list_);
    suggestions_frame_->hide();

    input_panel_ = new QFrame(central);
    auto *input_layout = new QGridLayout(input_panel_);
    input_layout->setContentsMargins(8, 4, 8, 4);
    input_layout->setHorizontalSpacing(6);
    input_layout->setVerticalSpacing(4);

    input_r1_label_ = new QLabel("R1", input_panel_);
    input_call_r1_label_ = new QLabel("Call", input_panel_);
    input_call_r1_edit_ = new QLineEdit(input_panel_);
    input_rst_r1_label_ = new QLabel("Exchange", input_panel_);
    input_rst_r1_edit_ = new QLineEdit(input_panel_);
    input_comments_r1_label_ = new QLabel("Comments", input_panel_);
    input_comments_r1_edit_ = new QLineEdit(input_panel_);

    input_r2_label_ = new QLabel("R2", input_panel_);
    input_call_r2_label_ = new QLabel("Call", input_panel_);
    input_call_r2_edit_ = new QLineEdit(input_panel_);
    input_rst_r2_label_ = new QLabel("Exchange", input_panel_);
    input_rst_r2_edit_ = new QLineEdit(input_panel_);
    input_comments_r2_label_ = new QLabel("Comments", input_panel_);
    input_comments_r2_edit_ = new QLineEdit(input_panel_);

    auto prepare_input = [](QLineEdit *edit) {
      edit->setReadOnly(true);
      edit->setFocusPolicy(Qt::NoFocus);
    };
    prepare_input(input_call_r1_edit_);
    prepare_input(input_rst_r1_edit_);
    prepare_input(input_comments_r1_edit_);
    prepare_input(input_call_r2_edit_);
    prepare_input(input_rst_r2_edit_);
    prepare_input(input_comments_r2_edit_);

    input_call_r1_edit_->setMinimumWidth(220);
    input_rst_r1_edit_->setMinimumWidth(140);
    input_comments_r1_edit_->setMinimumWidth(280);
    input_call_r2_edit_->setMinimumWidth(220);
    input_rst_r2_edit_->setMinimumWidth(140);
    input_comments_r2_edit_->setMinimumWidth(280);

    input_comments_r1_label_->hide();
    input_comments_r1_edit_->hide();
    input_comments_r2_label_->hide();
    input_comments_r2_edit_->hide();

    input_layout->addWidget(input_r1_label_, 0, 0);
    input_layout->addWidget(input_call_r1_label_, 0, 1);
    input_layout->addWidget(input_call_r1_edit_, 0, 2);
    input_layout->addWidget(input_rst_r1_label_, 0, 3);
    input_layout->addWidget(input_rst_r1_edit_, 0, 4);

    input_layout->addWidget(input_r2_label_, 1, 0);
    input_layout->addWidget(input_call_r2_label_, 1, 1);
    input_layout->addWidget(input_call_r2_edit_, 1, 2);
    input_layout->addWidget(input_rst_r2_label_, 1, 3);
    input_layout->addWidget(input_rst_r2_edit_, 1, 4);

    input_layout->setColumnStretch(2, 3);
    input_layout->setColumnStretch(4, 2);
    left_layout->addWidget(input_panel_);

    status_panel_ = new QFrame(central);
    auto *status_layout = new QHBoxLayout(status_panel_);
    status_layout->setContentsMargins(8, 2, 8, 2);
    status_label_ = new QLabel(status_panel_);
    info_label_ = new QLabel(status_panel_);
    status_label_->setWordWrap(false);
    info_label_->setWordWrap(false);
    status_layout->addWidget(status_label_, 1);
    status_layout->addWidget(info_label_, 1, Qt::AlignRight);
    left_layout->addWidget(status_panel_);

    dxcc_panel_ = new QFrame(central);
    auto *dxcc_layout = new QHBoxLayout(dxcc_panel_);
    dxcc_layout->setContentsMargins(8, 2, 8, 2);
    dxcc_label_ = new QLabel(dxcc_panel_);
    dxcc_label_->setWordWrap(false);
    dxcc_layout->addWidget(dxcc_label_);
    left_layout->addWidget(dxcc_panel_);

    stats_panel_ = new QFrame(central);
    auto *stats_layout = new QHBoxLayout(stats_panel_);
    stats_layout->setContentsMargins(8, 2, 8, 2);
    stats_label_ = new QLabel(stats_panel_);
    stats_label_->setWordWrap(false);
    stats_layout->addWidget(stats_label_);
    left_layout->addWidget(stats_panel_);

        op_panel_ = new QFrame(central);
        auto *op_layout = new QGridLayout(op_panel_);
        op_layout->setContentsMargins(8, 4, 8, 4);
        op_layout->setHorizontalSpacing(8);
        op_layout->setVerticalSpacing(3);

        op_layout->addWidget(new QLabel("Technique:", op_panel_), 0, 0);
        technique_combo_ = new QComboBox(op_panel_);
        technique_combo_->addItems({"SO1R", "SO2V", "SO2R"});
        op_layout->addWidget(technique_combo_, 0, 1);

        op_layout->addWidget(new QLabel("Focus:", op_panel_), 0, 2);
        radio1_focus_button_ = new QPushButton("Radio 1", op_panel_);
        radio2_focus_button_ = new QPushButton("Radio 2", op_panel_);
        swap_radios_button_ = new QPushButton("Swap", op_panel_);
        op_layout->addWidget(radio1_focus_button_, 0, 3);
        op_layout->addWidget(radio2_focus_button_, 0, 4);
        op_layout->addWidget(swap_radios_button_, 0, 5);

        op_layout->addWidget(new QLabel("Run/S&P:", op_panel_), 1, 2);
        run1_toggle_button_ = new QPushButton("R1 RUN", op_panel_);
        run2_toggle_button_ = new QPushButton("R2 S&P", op_panel_);
        op_layout->addWidget(run1_toggle_button_, 1, 3);
        op_layout->addWidget(run2_toggle_button_, 1, 4);

        radio1_label_ = new QLabel("R1 --", op_panel_);
        radio2_label_ = new QLabel("R2 --", op_panel_);
        op_layout->addWidget(radio1_label_, 1, 0, 1, 2);
        op_layout->addWidget(radio2_label_, 2, 0, 1, 2);

        op_layout->addWidget(new QLabel("TX Exchange:", op_panel_), 2, 2);
        contest_tx_exchange_edit_ = new QLineEdit(op_panel_);
        contest_tx_exchange_edit_->setText(
          QString::fromLatin1(config.contest_tx_exchange));
        contest_tx_exchange_edit_->setPlaceholderText("auto (from EXCHANGE_SENT)");
        contest_tx_exchange_edit_->setToolTip(
          "Overrides TX exchange for static contest templates like ITU or CQZONE.");
        op_layout->addWidget(contest_tx_exchange_edit_, 2, 3, 1, 3);

        connect(technique_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, [this](int) { on_technique_changed(); });
        connect(radio1_focus_button_, &QPushButton::clicked, this,
          [this]() { on_focus_radio(1); });
        connect(radio2_focus_button_, &QPushButton::clicked, this,
          [this]() { on_focus_radio(2); });
        connect(swap_radios_button_, &QPushButton::clicked, this,
          [this]() { on_swap_radios(); });
        connect(run1_toggle_button_, &QPushButton::clicked, this,
          [this]() { on_toggle_run(1); });
        connect(run2_toggle_button_, &QPushButton::clicked, this,
          [this]() { on_toggle_run(2); });
        connect(contest_tx_exchange_edit_, &QLineEdit::editingFinished, this,
          [this]() { on_contest_tx_exchange_changed(); });
        left_layout->addWidget(op_panel_);

    gap_panel_ = new QFrame(central);
    gap_panel_->setFixedHeight(10);
    left_layout->addWidget(gap_panel_);

    cluster_group_ = new QGroupBox("DX Cluster", central);
    cluster_group_->setObjectName("clusterGroup");
    auto *cluster_layout = new QVBoxLayout(cluster_group_);
    cluster_layout->setContentsMargins(6, 24, 6, 6);
    cluster_layout->setSpacing(4);

        auto *cluster_search_row = new QFrame(cluster_group_);
        auto *cluster_search_layout = new QHBoxLayout(cluster_search_row);
        cluster_search_layout->setContentsMargins(0, 0, 0, 0);
        cluster_search_layout->setSpacing(6);
        auto *cluster_search_label = new QLabel("Call:", cluster_search_row);
        cluster_search_edit_ = new QLineEdit(cluster_search_row);
        cluster_search_edit_->setPlaceholderText("Search callsign...");
        cluster_search_edit_->setClearButtonEnabled(true);
        connect(cluster_search_edit_, &QLineEdit::textChanged, this,
          [this]() { refresh_ui(); });
        cluster_search_layout->addWidget(cluster_search_label);
        cluster_search_layout->addWidget(cluster_search_edit_, 1);
        cluster_layout->addWidget(cluster_search_row);

    auto *cluster_filters = new QFrame(cluster_group_);
    auto *cluster_filters_layout = new QGridLayout(cluster_filters);
    cluster_filters_layout->setContentsMargins(0, 0, 0, 0);
    cluster_filters_layout->setHorizontalSpacing(10);
    cluster_filters_layout->setVerticalSpacing(2);

    for (int i = 0; i < (int)kBandLabels.size(); i++) {
      cluster_band_checks_[i] = new QCheckBox(kBandLabels[i], cluster_filters);
      cluster_band_checks_[i]->setChecked(true);
      connect(cluster_band_checks_[i], &QCheckBox::toggled, this,
              [this]() { refresh_ui(); });
      cluster_filters_layout->addWidget(cluster_band_checks_[i], i / 4, i % 4);
    }

    cluster_layout->addWidget(cluster_filters);

    cluster_table_ = new QTableWidget(cluster_group_);
    cluster_table_->setColumnCount(4);
    cluster_table_->setHorizontalHeaderLabels({"Time", "Freq", "Call", "Comment"});
    cluster_table_->verticalHeader()->setVisible(false);
    cluster_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    cluster_table_->setSelectionMode(QAbstractItemView::NoSelection);
    cluster_table_->setFocusPolicy(Qt::NoFocus);
    cluster_table_->setShowGrid(false);
    cluster_table_->setWordWrap(false);
    cluster_table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    cluster_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    cluster_table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    cluster_layout->addWidget(cluster_table_);
        cluster_group_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

        cat_group_ = new QGroupBox("CAT (Hamlib)", central);
        cat_group_->setObjectName("catGroup");
        auto *cat_layout = new QVBoxLayout(cat_group_);
        cat_layout->setContentsMargins(6, 24, 6, 6);
        cat_layout->setSpacing(4);

        auto *cat_grid = new QGridLayout();
        cat_grid->setContentsMargins(0, 0, 0, 0);
        cat_grid->setHorizontalSpacing(8);
        cat_grid->setVerticalSpacing(4);

        int row = 0;
        cat_grid->addWidget(new QLabel("Radio:", cat_group_), row, 0);
        cat_slot_combo_ = new QComboBox(cat_group_);
        cat_slot_combo_->addItem("Radio 1 (CAT A)", CAT_SLOT_A);
        cat_slot_combo_->addItem("Radio 2 (CAT B)", CAT_SLOT_B);
        cat_grid->addWidget(cat_slot_combo_, row, 1);
        row++;

        cat_grid->addWidget(new QLabel("Rig:", cat_group_), row, 0);
        cat_rig_combo_ = new QComboBox(cat_group_);
        cat_grid->addWidget(cat_rig_combo_, row, 1);
        row++;

        cat_grid->addWidget(new QLabel("Device:", cat_group_), row, 0);
        cat_device_edit_ = new QLineEdit(cat_group_);
        cat_device_edit_->setText(QString::fromLatin1(config.cat_device));
        cat_grid->addWidget(cat_device_edit_, row, 1);
        row++;

        cat_grid->addWidget(new QLabel("Baud:", cat_group_), row, 0);
        cat_baud_combo_ = new QComboBox(cat_group_);
        cat_baud_combo_->addItems({"1200", "2400", "4800", "9600", "19200", "38400", "57600", "115200"});
        cat_baud_combo_->setEditable(true);
        cat_baud_combo_->setCurrentText(QString::number(config.cat_baud));
        cat_grid->addWidget(cat_baud_combo_, row, 1);
        row++;

        cat_grid->addWidget(new QLabel("Data bits:", cat_group_), row, 0);
        cat_data_bits_combo_ = new QComboBox(cat_group_);
        cat_data_bits_combo_->addItems({"5", "6", "7", "8"});
        cat_data_bits_combo_->setCurrentText(QString::number(config.cat_data_bits));
        cat_grid->addWidget(cat_data_bits_combo_, row, 1);
        row++;

        cat_grid->addWidget(new QLabel("Stop bits:", cat_group_), row, 0);
        cat_stop_bits_combo_ = new QComboBox(cat_group_);
        cat_stop_bits_combo_->addItems({"1", "2"});
        cat_stop_bits_combo_->setCurrentText(QString::number(config.cat_stop_bits));
        cat_grid->addWidget(cat_stop_bits_combo_, row, 1);
        row++;

        cat_grid->addWidget(new QLabel("Parity:", cat_group_), row, 0);
        cat_parity_combo_ = new QComboBox(cat_group_);
        cat_parity_combo_->addItems({"None", "Even", "Odd"});
        cat_parity_combo_->setCurrentText(QString::fromLatin1(config.cat_parity));
        cat_grid->addWidget(cat_parity_combo_, row, 1);
        row++;

        cat_grid->addWidget(new QLabel("Handshake:", cat_group_), row, 0);
        cat_handshake_combo_ = new QComboBox(cat_group_);
        cat_handshake_combo_->addItems({"None", "RTSCTS", "XONXOFF"});
        cat_handshake_combo_->setCurrentText(QString::fromLatin1(config.cat_handshake));
        cat_grid->addWidget(cat_handshake_combo_, row, 1);
        row++;

        cat_grid->addWidget(new QLabel("Mode from rig:", cat_group_), row, 0);
        cat_mode_from_rig_check_ = new QCheckBox(cat_group_);
        cat_mode_from_rig_check_->setChecked(config.cat_mode_from_rig != 0);
        cat_grid->addWidget(cat_mode_from_rig_check_, row, 1);

        cat_layout->addLayout(cat_grid);

        auto *cat_buttons = new QHBoxLayout();
        cat_connect_button_ = new QPushButton("Connect", cat_group_);
        cat_disconnect_button_ = new QPushButton("Disconnect", cat_group_);
        cat_buttons->addWidget(cat_connect_button_);
        cat_buttons->addWidget(cat_disconnect_button_);
        cat_layout->addLayout(cat_buttons);

        cat_status_frame_ = new QFrame(cat_group_);
        auto *cat_status_layout = new QHBoxLayout(cat_status_frame_);
        cat_status_layout->setContentsMargins(6, 4, 6, 4);
        cat_status_label_ = new QLabel("CAT status: idle", cat_status_frame_);
        cat_slot1_status_label_ = new QLabel("R1: idle", cat_status_frame_);
        cat_slot2_status_label_ = new QLabel("R2: idle", cat_status_frame_);
        cat_status_layout->addWidget(cat_status_label_);
        cat_status_layout->addWidget(cat_slot1_status_label_);
        cat_status_layout->addWidget(cat_slot2_status_label_);
        cat_layout->addWidget(cat_status_frame_);

        /* --- CW Keyer section --- */
        auto *cw_sep = new QFrame(cat_group_);
        cw_sep->setFrameShape(QFrame::HLine);
        cw_sep->setFrameShadow(QFrame::Sunken);
        cat_layout->addWidget(cw_sep);

        auto *cw_grid = new QGridLayout();
        cw_grid->setContentsMargins(0, 0, 0, 0);
        cw_grid->setHorizontalSpacing(8);
        cw_grid->setVerticalSpacing(4);

        cw_grid->addWidget(new QLabel("CW port:", cat_group_), 0, 0);
        cw_device_edit_ = new QLineEdit(cat_group_);
        cw_device_edit_->setText(QString::fromLatin1(config.cw_device));
        cw_grid->addWidget(cw_device_edit_, 0, 1);

        cw_grid->addWidget(new QLabel("Linia:", cat_group_), 1, 0);
        cw_line_combo_ = new QComboBox(cat_group_);
        cw_line_combo_->addItems({"DTR", "RTS"});
        cw_line_combo_->setCurrentText(QString::fromLatin1(config.cw_keyer_line));
        cw_grid->addWidget(cw_line_combo_, 1, 1);

        cw_grid->addWidget(new QLabel("WPM:", cat_group_), 2, 0);
        cw_wpm_spin_ = new QSpinBox(cat_group_);
        cw_wpm_spin_->setRange(5, 60);
        cw_wpm_spin_->setValue(config.cw_wpm);
        cw_grid->addWidget(cw_wpm_spin_, 2, 1);

        cat_layout->addLayout(cw_grid);

        auto *cw_buttons = new QHBoxLayout();
        cw_connect_button_ = new QPushButton("CW Connect", cat_group_);
        cw_disconnect_button_ = new QPushButton("CW Disconnect", cat_group_);
        cw_buttons->addWidget(cw_connect_button_);
        cw_buttons->addWidget(cw_disconnect_button_);
        cat_layout->addLayout(cw_buttons);

        cw_status_label_ = new QLabel("CW keyer: idle", cat_group_);
        cat_layout->addWidget(cw_status_label_);

        cat_layout->addStretch(1);

        connect(cat_connect_button_, &QPushButton::clicked, this,
          [this]() { on_cat_connect(); });
        connect(cat_disconnect_button_, &QPushButton::clicked, this,
          [this]() { on_cat_disconnect(); });
        connect(cat_slot_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, [this](int) { load_cat_slot_form(selected_cat_slot()); });
        connect(cw_connect_button_, &QPushButton::clicked, this,
          [this]() { on_cw_connect(); });
        connect(cw_disconnect_button_, &QPushButton::clicked, this,
          [this]() { on_cw_disconnect(); });

        load_cat_slot_form(CAT_SLOT_A);
        cat_group_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

        auto *cluster_cat_row = new QWidget(central);
        auto *cluster_cat_layout = new QHBoxLayout(cluster_cat_row);
        cluster_cat_layout->setContentsMargins(0, 0, 0, 0);
        cluster_cat_layout->setSpacing(6);
        cluster_cat_layout->addWidget(cluster_group_, 2);
        cluster_cat_layout->addWidget(cat_group_, 1);
        left_layout->addWidget(cluster_cat_row, 1);

    root->addWidget(left_column, 1);

    function_panel_ = new QFrame(central);
    auto *function_layout = new QHBoxLayout(function_panel_);
    function_layout->setContentsMargins(8, 2, 8, 2);
    function_label_ = new QLabel(function_panel_);
    function_label_->setWordWrap(false);
    function_layout->addWidget(function_label_);
    left_layout->addWidget(function_panel_);

    apply_theme();
    apply_character_cell_layout();

    cw_dialog_ = new CwKeyerDialog(this);

    /* try app dir (build), then CWD, then one level up (source tree) */
    {
      const QString base = QApplication::applicationDirPath();
      if (cw_keys_load(&cw_key_map_, (base + "/cw_keys.ini").toLocal8Bit().constData()) != 0 &&
          cw_keys_load(&cw_key_map_, "cw_keys.ini") != 0)
        cw_keys_load(&cw_key_map_, (base + "/../cw_keys.ini").toLocal8Bit().constData());
    }

    /* auto-connect CW keyer if device is configured */
    if (config.cw_device[0])
      cat_connect_cw_keyer(config.cw_device, config.cw_keyer_line, config.cw_wpm);

    timer_ = new QTimer(this);
    connect(timer_, &QTimer::timeout, this, [this]() { refresh_ui(); });
    timer_->start(120);

    refresh_ui();
    setFocus(Qt::ActiveWindowFocusReason);
  }

protected:
  void showEvent(QShowEvent *event) override {
    QMainWindow::showEvent(event);
    setFocus(Qt::ActiveWindowFocusReason);
  }

  /*
   * Recompute the character-cell layout after a window resize.
   *
   * @param event Resize event from Qt.
   * @return Nothing.
   */
  void resizeEvent(QResizeEvent *event) override {
    QMainWindow::resizeEvent(event);
    apply_character_cell_layout();
    place_suggestions_panel();
  }

  /*
   * Translate key presses into controller actions.
   *
   * @param event Key event from Qt.
   * @return Nothing.
   */
  void keyPressEvent(QKeyEvent *event) override {
    const bool ctrl = (event->modifiers() & Qt::ControlModifier) != 0;

    if (ctrl && event->key() == Qt::Key_F2) {
      prompt_create_named_log();
      refresh_ui();
      event->accept();
      return;
    }

    if (ctrl && event->key() == Qt::Key_F3) {
      prompt_open_named_log();
      refresh_ui();
      event->accept();
      return;
    }

    if (ctrl && event->key() == Qt::Key_K) {
      cw_dialog_->showAndFocus();
      event->accept();
      return;
    }

    if (event->key() == Qt::Key_Escape) {
      cat_cw_stop();
      /* fall through — ESC also clears input fields via APP_KEY_ESC */
    }

    if (!ctrl) {
      int fn = cw_fn_number(event->key());
      if (fn > 0) {
        on_cw_fn_key(fn);
        event->accept();
        return;
      }
    }

    int key = translate_key(event);
    if (key == APP_KEY_NONE) {
      event->ignore();
      return;
    }

    AppControllerEvent ctrl_event = app_controller_handle_key(key);
    if (ctrl_event == APP_CTRL_EVENT_REQUEST_CTY_UPDATE) {
      refresh_ui();
      qApp->processEvents();
      app_controller_perform_cty_update();
    }

    if (ctrl_event == APP_CTRL_EVENT_EXIT)
      close();

    refresh_ui();
    event->accept();
  }

private:
  void submit_command_text(const QString &command_text) {
    // Ensure command entry starts from a clean state; otherwise command text can
    // be appended to partially typed QSO input and won't be recognized.
    app_controller_handle_key(APP_KEY_ESC);

    AppRenderState state;
    app_controller_get_render_state(&state);
    if (state.input) {
      size_t existing_len = std::strlen(state.input);
      for (size_t i = 0; i < existing_len; i++)
        app_controller_handle_key(APP_KEY_BACKSPACE);
    }

    const QByteArray bytes = command_text.toUtf8();
    for (unsigned char ch : bytes)
      app_controller_handle_key((int)ch);

    app_controller_handle_key(APP_KEY_ENTER);
  }

  void prompt_create_named_log() {
    bool ok = false;
    const QString name = QInputDialog::getText(
        this,
        "Create New Log",
        "Enter new log name:",
        QLineEdit::Normal,
        "",
        &ok);

    if (!ok)
      return;

    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
      QMessageBox::information(this, "Create New Log",
                               "Log name cannot be empty.");
      return;
    }

    QStringList contest_options;
    contest_options << "None";
    for (const auto &preset : kContestPresets)
      contest_options << QString::fromLatin1(preset.label);

    const QString selected_contest = QInputDialog::getItem(
        this,
        "Create New Log",
        "Select contest:",
        contest_options,
        0,
        false,
        &ok);

    if (!ok || selected_contest.isEmpty())
      return;

    submit_command_text(QString("newlog %1").arg(trimmed));

    if (selected_contest == "None") {
      submit_command_text("contest none");
      return;
    }

    for (const auto &preset : kContestPresets) {
      if (selected_contest == QString::fromLatin1(preset.label)) {
        submit_command_text(
            QString("contest %1").arg(QString::fromLatin1(preset.path)));
        return;
      }
    }
  }

  void prompt_open_named_log() {
    DBNamedLogbook items[128];
    int count = 0;

    if (db_list_named_logbooks(items, 128, &count) != 0) {
      QMessageBox::warning(this, "Open Log",
                           "Cannot read logs from database.");
      return;
    }

    if (count <= 0) {
      QMessageBox::information(this, "Open Log",
                               "No saved logs available in database.");
      return;
    }

    QStringList options;
    for (int i = 0; i < count; i++) {
      options << QString("%1 | %2 | %3 QSO | %4")
                     .arg(items[i].id)
                     .arg(items[i].name)
                     .arg(items[i].qso_count)
                     .arg(items[i].created_at);
    }

    bool ok = false;
    const QString chosen = QInputDialog::getItem(
        this,
        "Open Log",
        "Select log to open:",
        options,
        0,
        false,
        &ok);

    if (!ok || chosen.isEmpty())
      return;

    const QString id = chosen.section(" | ", 0, 0).trimmed();
    if (id.isEmpty()) {
      QMessageBox::warning(this, "Open Log",
                           "Invalid log selection.");
      return;
    }

    submit_command_text(QString("openlog %1").arg(id));
  }

  static int translate_key(QKeyEvent *event) {
    const bool ctrl = (event->modifiers() & Qt::ControlModifier) != 0;
    switch (event->key()) {
    case Qt::Key_F1:
      return ctrl ? APP_KEY_F1 : APP_KEY_NONE;
    case Qt::Key_F2:
      return ctrl ? APP_KEY_F2 : APP_KEY_NONE;
    case Qt::Key_F3:
      return ctrl ? APP_KEY_F3 : APP_KEY_NONE;
    case Qt::Key_F4:
      return ctrl ? APP_KEY_F4 : APP_KEY_NONE;
    case Qt::Key_F5:
      return ctrl ? APP_KEY_F5 : APP_KEY_NONE;
    case Qt::Key_F6:
      return ctrl ? APP_KEY_F6 : APP_KEY_NONE;
    case Qt::Key_F7:
      return ctrl ? APP_KEY_F7 : APP_KEY_NONE;
    case Qt::Key_F10:
      return ctrl ? APP_KEY_F10 : APP_KEY_NONE;
    case Qt::Key_Up:
      return APP_KEY_UP;
    case Qt::Key_Down:
      return APP_KEY_DOWN;
    case Qt::Key_Left:
      return APP_KEY_LEFT;
    case Qt::Key_Right:
      return APP_KEY_RIGHT;
    case Qt::Key_PageUp:
      return APP_KEY_PAGE_UP;
    case Qt::Key_PageDown:
      return APP_KEY_PAGE_DOWN;
    case Qt::Key_Backspace:
      return APP_KEY_BACKSPACE;
    case Qt::Key_Return:
    case Qt::Key_Enter:
      return APP_KEY_ENTER;
    case Qt::Key_Tab:
      return APP_KEY_TAB;
    case Qt::Key_Escape:
      return APP_KEY_ESC;
    case Qt::Key_Space:
      return APP_KEY_SPACE;
    default:
      break;
    }

    if (!(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
      const QString text = event->text();
      if (text.size() == 1) {
        const QChar ch = text.at(0);
        if (ch.isPrint())
          return ch.toLatin1();
      }
    }

    return APP_KEY_NONE;
  }

  void apply_theme() {
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSize(10);
    setFont(mono);

    setStyleSheet(
      "QMainWindow, QWidget { background: #89d4e8; color: #0d2d3b; }"
        "QGroupBox { border: 1px solid #0e6a85; border-radius: 3px; margin-top: 8px;"
      "          background: #95dceb; font-weight: bold; }"
      "QGroupBox#logGroup::title { subcontrol-origin: margin; left: 10px;"
"                           padding: 0 6px; color: #f4f8ff; background: #0f5ea4; }"
      "QGroupBox#clusterGroup::title { subcontrol-origin: margin; left: 10px; padding: 0 6px;"
      "                               color: #f4f8ff; background: #0f5ea4; }"
      "QHeaderView::section { background: #0f5ea4; color: #ffe66d; padding: 1px 4px;"
        "                       border: 0; font-weight: bold; }"
      "QTableWidget { background: #9be2ef; gridline-color: #5ca9bf; }"
        "QLineEdit { background: #9be2ef; border: 1px solid #0e6a85; color: #0d2d3b; }"
        "QFrame#suggestTitle { color: #f4f8ff; }"
        "QListWidget { background: #9be2ef; border: 1px solid #0e6a85; }"
        "QListWidget::item:selected { background: #f3d24f; color: #111; }");

    input_panel_->setStyleSheet("QFrame { background: #9be2ef; border: 1px solid #0e6a85; }");
    status_panel_->setStyleSheet("QFrame { background: #0f5ea4; color: #f4f8ff; }");
    dxcc_panel_->setStyleSheet("QFrame { background: #0f5ea4; color: #ffe66d; font-weight: bold; }");
    stats_panel_->setStyleSheet("QFrame { background: #0f5ea4; color: #ffe66d; font-weight: bold; }");
    op_panel_->setStyleSheet(
      "QFrame { background: #95dceb; border: 1px solid #0e6a85; }"
      "QLabel { color: #0d2d3b; font-weight: bold; }");
    gap_panel_->setStyleSheet("QFrame { background: #95dceb; border: 0; }");
    function_panel_->setStyleSheet("QFrame { background: #0f5ea4; color: #f4f8ff; font-weight: bold; }");
    suggestions_frame_->setStyleSheet(
        "QFrame { background: #95dceb; border: 1px solid #0e6a85; }"
        "QLabel#suggestTitle { background: #0f5ea4; color: #f4f8ff; padding: 2px 6px; font-weight: bold; }");
    cluster_group_->setStyleSheet(
      "QGroupBox#clusterGroup::title { subcontrol-origin: margin; left: 10px; padding: 0 6px;"
      " color: #f4f8ff; background: #0f5ea4; }"
      "QCheckBox { color: #0d2d3b; font-weight: bold; }");
    cat_group_->setStyleSheet(
      "QGroupBox#catGroup::title { subcontrol-origin: margin; left: 10px; padding: 0 6px;"
      " color: #f4f8ff; background: #0f5ea4; }"
      "QLabel { color: #0d2d3b; font-weight: bold; }");
  }

  int cols_to_px(int cols, int padding = 10) const {
    return cols * cell_w_ + padding;
  }

  QString clip_to_cols(const QString &text, int cols) const {
    if (cols <= 0)
      return QString();

    return text.left(cols);
  }

  QString clip_cstr_to_cols(const char *text, int cols) const {
    if (!text || cols <= 0)
      return QString();

    const int n = std::min(cols, (int)std::strlen(text));
    return QString::fromLatin1(text, n);
  }

  int panel_available_cols(const QWidget *panel, int inner_padding_px = 16) const {
    if (!panel)
      return 0;

    const int px = std::max(0, panel->width() - inner_padding_px);
    return px / std::max(1, cell_w_);
  }

  void apply_character_cell_layout() {
    QFontMetrics fm(font());
    cell_w_ = std::max(7, fm.horizontalAdvance(QLatin1Char('M')));
    cell_h_ = std::max(14, fm.height());

    const int row_h = cell_h_ + 2;
    const int header_h = cell_h_ + 4;
    const int line_panel_h = cell_h_ + 8;

    log_table_->verticalHeader()->setDefaultSectionSize(row_h);
    log_table_->horizontalHeader()->setFixedHeight(header_h);
    cluster_table_->verticalHeader()->setDefaultSectionSize(row_h);
    cluster_table_->horizontalHeader()->setFixedHeight(header_h);

    const int log_col_chars[9] = {3, 9, 5, 15, 6, 5, 5, 14, 22};
    for (int i = 0; i < 9; i++) {
      if (i == 3 || i == 8)
        continue;
      log_table_->setColumnWidth(i, cols_to_px(log_col_chars[i]));
    }
    log_table_->horizontalHeader()->setSectionResizeMode(8, QHeaderView::Stretch);

    const int cluster_col_chars[4] = {9, 11, 13, 20};
    cluster_table_->setColumnWidth(0, cols_to_px(cluster_col_chars[0]));
    cluster_table_->setColumnWidth(1, cols_to_px(cluster_col_chars[1]));
    cluster_table_->setColumnWidth(2, cols_to_px(cluster_col_chars[2]));

    log_group_->setMinimumHeight((12 + 3) * row_h + 26);

    input_panel_->setFixedHeight(line_panel_h * 2 + 8);
    status_panel_->setFixedHeight(line_panel_h);
    dxcc_panel_->setFixedHeight(line_panel_h);
    stats_panel_->setFixedHeight(line_panel_h);
    op_panel_->setFixedHeight(line_panel_h * 3 + 12);
    gap_panel_->setFixedHeight(line_panel_h / 2);
    function_panel_->setFixedHeight(line_panel_h);
  }

  void place_suggestions_panel() {
    if (!log_group_)
      return;

    const int total_cols = std::max(1, log_group_->width() / std::max(1, cell_w_));
    const int suggest_cols = std::clamp(total_cols / 3, 24, 40);
    const int suggest_rows = 8;
    const int suggest_w = cols_to_px(suggest_cols, 14);
    const int suggest_h = (suggest_rows + 2) * (cell_h_ + 2) + 8;
    suggestions_frame_->setFixedSize(suggest_w, suggest_h);

    const int x = std::max(8, log_group_->width() - suggestions_frame_->width() - 12);
    const int y = 26;
    suggestions_frame_->move(x, y);
    suggestions_frame_->raise();
  }

  void refresh_log_table() {
    log_table_->setHorizontalHeaderLabels(
      {"Nr", "Date", "UTC", "Call", "Freq", "Band", "Mode", "Exch S/R", "Run/Pts"});

    if (qso_count <= 0) {
      log_table_->setRowCount(1);
      auto *item = new QTableWidgetItem("No QSOs");
      item->setTextAlignment(Qt::AlignCenter);
      log_table_->setSpan(0, 0, 1, log_table_->columnCount());
      log_table_->setItem(0, 0, item);
      return;
    }

    const int max_rows = 200;
    const int start = std::max(0, qso_count - max_rows);
    const int rows = qso_count - start;
    log_table_->clearSpans();
    log_table_->setRowCount(rows);

    for (int row = 0; row < rows; row++) {
      const int idx = start + row;
      const QSO &q = logbook[idx];

      auto set_item = [&](int col, const QString &text) {
        auto *cell = new QTableWidgetItem(text);
          const int hard_cols[9] = {3, 8, 4, 14, 5, 4, 4, 18, 24};
          cell->setText(clip_to_cols(cell->text(), hard_cols[col]));
          cell->setToolTip(text);
          cell->setTextAlignment(Qt::AlignCenter | Qt::AlignVCenter);

        if (q.invalid) {
          cell->setBackground(QColor(250, 224, 110));
          cell->setForeground(QColor(20, 20, 20));
        }
        log_table_->setItem(row, col, cell);
      };

      set_item(0, QString::number(idx + 1));
      set_item(1, q.date);
      set_item(2, q.utc);
      set_item(3, q.call);
      set_item(4, QString::number(q.freq));
      set_item(5, q.band);
      set_item(6, q.mode);
      const QString op_mode = QString::fromLatin1(q.operator_mode);
      const QString run_pts = QString("%1/%2")
                                .arg(op_mode.isEmpty() ? "--" : op_mode,
                                     QString::number(q.points));
      set_item(7, render_exchange_sr(q));
      set_item(8, run_pts);
    }

    log_table_->scrollToBottom();
  }

  int refresh_cluster_table(bool fullscreen_mode, int scroll) {
    struct SpotCopy {
      QString time;
      QString freq;
      QString call;
      QString comment;
    };

    std::vector<SpotCopy> items;
    QString status;
    bool band_enabled[(int)kBandLabels.size()];
    const QString call_filter =
      cluster_search_edit_ ? cluster_search_edit_->text().trimmed() : QString();

    for (int i = 0; i < (int)kBandLabels.size(); i++)
      band_enabled[i] =
          cluster_band_checks_[i] && cluster_band_checks_[i]->isChecked();

    pthread_mutex_lock(&dxcluster_mutex);
    status = dxcluster_status;

    int start = spot_start;
    int count = spot_count;
    int max_scroll = 0;

    if (!fullscreen_mode) {
      const int visible = 70;
      if (count > visible) {
        start = (spot_start + count - visible) % MAX_SPOTS;
        count = visible;
      }
    } else {
      int visible = cluster_table_->viewport()->height() /
                    std::max(1, cluster_table_->verticalHeader()->defaultSectionSize());
      if (visible < 1)
        visible = 1;

      max_scroll = count > visible ? count - visible : 0;
      if (scroll < 0)
        scroll = 0;
      if (scroll > max_scroll)
        scroll = max_scroll;

      const int offset = count > visible ? count - visible - scroll : 0;
      start = (spot_start + offset) % MAX_SPOTS;
      count = std::min(count, visible);
    }

    items.reserve(count);
    for (int i = 0; i < count; i++) {
      const int idx = (start + i) % MAX_SPOTS;
      const int freq_khz = parse_spot_khz(spots[idx].freq);
      if (freq_khz <= 0)
        continue;

      char band[8] = {0};
      detect_band(freq_khz, band);
      const int band_idx = band_index(band);
      if (band_idx < 0 || !band_enabled[band_idx])
        continue;

      if (!call_filter.isEmpty()) {
        const QString call = QString::fromLatin1(spots[idx].call);
        if (!call.contains(call_filter, Qt::CaseInsensitive))
          continue;
      }

      items.push_back({spots[idx].time, spots[idx].freq, spots[idx].call, spots[idx].comment});
    }
    pthread_mutex_unlock(&dxcluster_mutex);

    cluster_group_->setTitle(QString("DX Cluster [%1]").arg(status));

    cluster_table_->setRowCount((int)items.size());
    for (int row = 0; row < (int)items.size(); row++) {
      auto *time_item = new QTableWidgetItem(items[row].time);
      auto *freq_item = new QTableWidgetItem(items[row].freq);
      auto *call_item = new QTableWidgetItem(items[row].call);
      auto *comment_item = new QTableWidgetItem(items[row].comment);

      time_item->setText(clip_to_cols(time_item->text(), 8));
      freq_item->setText(clip_to_cols(freq_item->text(), 10));
      call_item->setText(clip_to_cols(call_item->text(), 12));
      comment_item->setText(clip_to_cols(comment_item->text(), 80));

      time_item->setTextAlignment(Qt::AlignCenter);
      freq_item->setTextAlignment(Qt::AlignCenter | Qt::AlignVCenter);
      call_item->setTextAlignment(Qt::AlignCenter | Qt::AlignVCenter);
      comment_item->setTextAlignment(Qt::AlignCenter | Qt::AlignVCenter);

      time_item->setForeground(QColor(14, 90, 20));
      freq_item->setForeground(QColor(14, 90, 20));
      call_item->setForeground(QColor(14, 90, 20));
      comment_item->setForeground(QColor(14, 90, 20));

      cluster_table_->setItem(row, 0, time_item);
      cluster_table_->setItem(row, 1, freq_item);
      cluster_table_->setItem(row, 2, call_item);
      cluster_table_->setItem(row, 3, comment_item);
    }

    if (!fullscreen_mode)
      cluster_table_->scrollToBottom();

    return max_scroll;
  }

  void refresh_suggestions() {
    if (!call_suggestion_available || call_suggestion_count <= 0) {
      suggestions_frame_->hide();
      input_panel_->setStyleSheet("QFrame { background: #9be2ef; border: 1px solid #0e6a85; }");
      return;
    }

    suggestions_frame_->show();
    place_suggestions_panel();

    input_panel_->setStyleSheet("QFrame { background: #f3d24f; border: 1px solid #0e6a85; }");

    suggestions_list_->clear();
    for (int i = 0; i < call_suggestion_count; i++)
      suggestions_list_->addItem(call_suggestion_matches[i]);

    if (call_suggestion_selected_index >= 0 &&
        call_suggestion_selected_index < suggestions_list_->count()) {
      suggestions_list_->setCurrentRow(call_suggestion_selected_index);
      suggestions_list_->scrollToItem(suggestions_list_->item(call_suggestion_selected_index));
    }
  }

  int selected_cat_slot() const {
    if (!cat_slot_combo_)
      return CAT_SLOT_A;

    return cat_slot_combo_->currentData().toInt();
  }

  void populate_cat_rigs(int slot) {
    cat_rig_combo_->clear();

    CatRigInfo rigs[256];
    const int count = cat_list_rigs(rigs, 256);
    if (count <= 0) {
      cat_rig_combo_->addItem("No rigs available", 0);
      return;
    }

    const int selected_model = slot == CAT_SLOT_B ? config.cat2_model : config.cat_model;

    int selected_index = 0;
    for (int i = 0; i < count; i++) {
      cat_rig_combo_->addItem(QString::fromLatin1(rigs[i].model_name), rigs[i].model);
      if (rigs[i].model == selected_model)
        selected_index = i;
    }

    cat_rig_combo_->setCurrentIndex(selected_index);
  }

  void load_cat_slot_form(int slot) {
    if (slot != CAT_SLOT_B)
      slot = CAT_SLOT_A;

    populate_cat_rigs(slot);

    if (slot == CAT_SLOT_B) {
      cat_device_edit_->setText(QString::fromLatin1(config.cat2_device));
      cat_baud_combo_->setCurrentText(QString::number(config.cat2_baud));
      cat_data_bits_combo_->setCurrentText(QString::number(config.cat2_data_bits));
      cat_stop_bits_combo_->setCurrentText(QString::number(config.cat2_stop_bits));
      cat_parity_combo_->setCurrentText(QString::fromLatin1(config.cat2_parity));
      cat_handshake_combo_->setCurrentText(QString::fromLatin1(config.cat2_handshake));
    } else {
      cat_device_edit_->setText(QString::fromLatin1(config.cat_device));
      cat_baud_combo_->setCurrentText(QString::number(config.cat_baud));
      cat_data_bits_combo_->setCurrentText(QString::number(config.cat_data_bits));
      cat_stop_bits_combo_->setCurrentText(QString::number(config.cat_stop_bits));
      cat_parity_combo_->setCurrentText(QString::fromLatin1(config.cat_parity));
      cat_handshake_combo_->setCurrentText(QString::fromLatin1(config.cat_handshake));
    }

    cat_mode_from_rig_check_->setChecked(config.cat_mode_from_rig != 0);
  }

  void refresh_cat_status() {
    char status[128] = {0};
    char slot1[128] = {0};
    char slot2[128] = {0};
    cat_get_status(status, sizeof(status));
    cat_get_status_slot(CAT_SLOT_A, slot1, sizeof(slot1));
    cat_get_status_slot(CAT_SLOT_B, slot2, sizeof(slot2));

    const bool connected = cat_is_connected_slot(CAT_SLOT_A) != 0 ||
                           cat_is_connected_slot(CAT_SLOT_B) != 0;
    cat_status_label_->setText(QString("CAT status: %1").arg(QString::fromLatin1(status)));
    cat_slot1_status_label_->setText(QString("R1: %1").arg(QString::fromLatin1(slot1)));
    cat_slot2_status_label_->setText(QString("R2: %1").arg(QString::fromLatin1(slot2)));
    cat_status_frame_->setStyleSheet(connected
                                         ? "QFrame { background: #87d37c; border: 1px solid #0e6a85; }"
                                         : "QFrame { background: #f3d24f; border: 1px solid #0e6a85; }");
  }

  void on_cat_connect() {
    const int slot = selected_cat_slot();
    CatConnectionParams params;
    std::memset(&params, 0, sizeof(params));

    params.model = cat_rig_combo_->currentData().toInt();
    std::snprintf(params.device, sizeof(params.device), "%s",
                  cat_device_edit_->text().trimmed().toLatin1().constData());
    params.baud_rate = cat_baud_combo_->currentText().toInt();
    params.data_bits = cat_data_bits_combo_->currentText().toInt();
    params.stop_bits = cat_stop_bits_combo_->currentText().toInt();
    std::snprintf(params.parity, sizeof(params.parity), "%s",
                  cat_parity_combo_->currentText().trimmed().toLatin1().constData());
    std::snprintf(params.handshake, sizeof(params.handshake), "%s",
                  cat_handshake_combo_->currentText().trimmed().toLatin1().constData());

    if (params.model <= 0) {
      QMessageBox::warning(this, "CAT", "Choose a valid rig model first.");
      return;
    }

    config.cat_mode_from_rig = cat_mode_from_rig_check_->isChecked() ? 1 : 0;
    if (slot == CAT_SLOT_B) {
      config.cat2_model = params.model;
      std::snprintf(config.cat2_device, sizeof(config.cat2_device), "%s", params.device);
      config.cat2_baud = params.baud_rate;
      config.cat2_data_bits = params.data_bits;
      config.cat2_stop_bits = params.stop_bits;
      std::snprintf(config.cat2_parity, sizeof(config.cat2_parity), "%s", params.parity);
      std::snprintf(config.cat2_handshake, sizeof(config.cat2_handshake), "%s", params.handshake);
    } else {
      config.cat_model = params.model;
      std::snprintf(config.cat_device, sizeof(config.cat_device), "%s", params.device);
      config.cat_baud = params.baud_rate;
      config.cat_data_bits = params.data_bits;
      config.cat_stop_bits = params.stop_bits;
      std::snprintf(config.cat_parity, sizeof(config.cat_parity), "%s", params.parity);
      std::snprintf(config.cat_handshake, sizeof(config.cat_handshake), "%s", params.handshake);
    }

    if (config_save("logger.conf") != 0) {
      QMessageBox::warning(this, "CAT",
                           "Failed to save logger.conf. CAT settings will not persist.");
    }

    if (cat_connect_slot(slot, &params) != 0) {
      refresh_cat_status();
      QMessageBox::warning(this, "CAT", "CAT connection failed. Check status bar and parameters.");
      return;
    }

    refresh_cat_status();
  }

  void on_cat_disconnect() {
    cat_disconnect_slot(selected_cat_slot());
    refresh_cat_status();
  }

  static int cw_fn_number(int qt_key) {
    switch (qt_key) {
    case Qt::Key_F1:  return 1;
    case Qt::Key_F2:  return 2;
    case Qt::Key_F3:  return 3;
    case Qt::Key_F4:  return 4;
    case Qt::Key_F5:  return 5;
    case Qt::Key_F6:  return 6;
    case Qt::Key_F7:  return 7;
    case Qt::Key_F8:  return 8;
    case Qt::Key_F9:  return 9;
    case Qt::Key_F10: return 10;
    default:          return 0;
    }
  }

  void on_cw_fn_key(int fn) {
    AppRenderState state;
    app_controller_get_render_state(&state);

    const int active = state.active_radio;
    const bool is_run = (active == 2) ? state.radio2_run : state.radio1_run;
    const char *mode  = (active == 2) ? state.radio2_mode : state.radio1_mode;

    const char *rst  = (mode && std::strstr(mode, "CW")) ? "599" : "59";
    const char *exch = state.contest_exchange_sent ? state.contest_exchange_sent : "";

    const char *tpl = cw_keys_get(&cw_key_map_, fn, is_run ? 1 : 0);
    if (!tpl || tpl[0] == 0) {
      cw_feedback_ = QString("F%1: brak definicji CW").arg(fn);
      cw_feedback_ticks_ = 20;
      return;
    }

    char text[256];
    cw_keys_expand(tpl, config.station_call, rst, exch, text, sizeof(text));
    if (text[0] == 0) return;

    cw_feedback_ = QString("CW: %1").arg(QString::fromLatin1(text));
    cw_feedback_ticks_ = 20;

    if (cat_is_cw_keyer_connected()) {
      cat_cw_send(text);
    } else if (cat_send_morse_slot(CAT_SLOT_A, text) != 0) {
      cw_feedback_ = QString("CW: brak keyer/CAT — %1").arg(QString::fromLatin1(text));
    }
  }

  void on_cw_connect() {    const QString device = cw_device_edit_->text().trimmed();
    const QString line   = cw_line_combo_->currentText();
    const int wpm        = cw_wpm_spin_->value();

    std::snprintf(config.cw_device, sizeof(config.cw_device), "%s",
                  device.toLatin1().constData());
    std::snprintf(config.cw_keyer_line, sizeof(config.cw_keyer_line), "%s",
                  line.toLatin1().constData());
    config.cw_wpm = wpm;
    config_save("logger.conf");

    if (cat_connect_cw_keyer(config.cw_device, config.cw_keyer_line,
                             config.cw_wpm) != 0) {
      QMessageBox::warning(this, "CW Keyer",
                           "Nie można otworzyć portu CW. Sprawdź ścieżkę urządzenia.");
    }
    refresh_cw_keyer_status();
  }

  void on_cw_disconnect() {
    cat_disconnect_cw_keyer();
    refresh_cw_keyer_status();
  }

  void refresh_cw_keyer_status() {
    char status[128] = {0};
    cat_get_cw_keyer_status(status, sizeof(status));
    cw_status_label_->setText(QString::fromLatin1(status));
    const bool connected = cat_is_cw_keyer_connected() != 0;
    cw_status_label_->setStyleSheet(connected
        ? "QLabel { color: #175017; font-weight: bold; }"
        : "QLabel { color: #555; }");
  }

  void on_technique_changed() {
    app_controller_set_contest_technique(
        technique_from_combo_index(technique_combo_->currentIndex()));
    refresh_ui();
  }

  void on_focus_radio(int radio_nr) {
    app_controller_set_active_radio(radio_nr);
    refresh_ui();
  }

  void on_swap_radios() {
    app_controller_swap_radios();
    refresh_ui();
  }

  void on_toggle_run(int radio_nr) {
    app_controller_toggle_run_sp(radio_nr);
    refresh_ui();
  }

  void on_contest_tx_exchange_changed() {
    if (!contest_tx_exchange_edit_)
      return;

    const QString trimmed = contest_tx_exchange_edit_->text().trimmed();
    std::snprintf(config.contest_tx_exchange, sizeof(config.contest_tx_exchange),
                  "%s", trimmed.toLatin1().constData());

    if (config_save("logger.conf") != 0) {
      QMessageBox::warning(this, "Config",
                           "Failed to save logger.conf. TX exchange override will not persist.");
    }

    refresh_ui();
  }

  /*
   * Pull the latest controller state into the Qt widgets.
   *
   * @return Nothing.
   */
  void refresh_ui() {
    AppRenderState state;
    app_controller_get_render_state(&state);
    const int active_freq_khz = app_controller_get_active_frequency_khz();
    log_group_->setTitle(QString("QSO Log [%1 kHz]").arg(active_freq_khz));

    const int input_call_cols_r1 =
      std::max(1, input_call_r1_edit_->width() / std::max(1, cell_w_) - 2);
    const int input_rst_cols_r1 =
      std::max(1, input_rst_r1_edit_->width() / std::max(1, cell_w_) - 2);
    const int input_call_cols_r2 =
      std::max(1, input_call_r2_edit_->width() / std::max(1, cell_w_) - 2);
    const int input_rst_cols_r2 =
      std::max(1, input_rst_r2_edit_->width() / std::max(1, cell_w_) - 2);
    const int status_cols = panel_available_cols(status_panel_, 16) / 2;
    const int info_cols = panel_available_cols(status_panel_, 16) / 2;
    const int dxcc_cols = panel_available_cols(dxcc_panel_, 16);
    const int stats_cols = panel_available_cols(stats_panel_, 16);
    const int func_cols = panel_available_cols(function_panel_, 16);

    input_call_r1_edit_->setText(clip_cstr_to_cols(state.input_call_r1, input_call_cols_r1));
    input_rst_r1_edit_->setText(clip_cstr_to_cols(state.input_rst_r1, input_rst_cols_r1));
    input_call_r2_edit_->setText(clip_cstr_to_cols(state.input_call_r2, input_call_cols_r2));
    input_rst_r2_edit_->setText(clip_cstr_to_cols(state.input_rst_r2, input_rst_cols_r2));

    auto set_input_style = [](QLineEdit *edit, bool active_field) {
      edit->setStyleSheet(active_field
                              ? "QLineEdit { background: #f3d24f; border: 1px solid #0e6a85; color: #111; }"
                              : "QLineEdit { background: #9be2ef; border: 1px solid #0e6a85; color: #0d2d3b; }");
    };

    const int active_radio = app_controller_get_active_radio();
    const bool contest_entry_mode = state.contest_entry_mode;
    set_input_style(input_call_r1_edit_, active_radio == 1 && state.active_input_field_r1 == 0);
    set_input_style(input_rst_r1_edit_, active_radio == 1 && state.active_input_field_r1 == 1);
    set_input_style(input_call_r2_edit_, active_radio == 2 && state.active_input_field_r2 == 0);
    set_input_style(input_rst_r2_edit_, active_radio == 2 && state.active_input_field_r2 == 1);
    status_label_->setText(clip_to_cols(
      cw_feedback_ticks_ > 0
        ? (cw_feedback_ticks_--, cw_feedback_)
        : QString("Status: %1").arg(state.status ? state.status : ""),
      std::max(1, status_cols)));
    QString info_text = QString("Info: %1").arg(state.info ? state.info : "");
    if (contest_entry_mode) {
      const QString exchange_label =
        QString::fromLatin1(state.contest_exchange_label ? state.contest_exchange_label : "Exchange");
      const QString exchange_sent =
        QString::fromLatin1(state.contest_exchange_sent ? state.contest_exchange_sent : "");
      info_text = QString("%1 | TX %2: %3").arg(info_text, exchange_label, exchange_sent);
    }
    info_label_->setText(clip_to_cols(info_text, std::max(1, info_cols)));
    dxcc_label_->setText(clip_to_cols(QString("DXCC: %1  CQ:%2 ITU:%3")
                        .arg(state.dxcc ? state.dxcc : "")
                             .arg(last_cq)
                        .arg(last_itu),
                      std::max(1, dxcc_cols)));
    stats_label_->setText(clip_to_cols(
      QString("QSO:%1 DXCC:%2  CW:%3 SSB:%4 FT8:%5 FT4:%6 RTTY:%7 PSK31:%8  PTS:%9 MULT:%10 SCORE:%11")
        .arg(stats.total_qso)
        .arg(stats.total_dxcc)
        .arg(stats.cw)
        .arg(stats.ssb)
        .arg(stats.ft8)
        .arg(stats.ft4)
        .arg(stats.rtty)
        .arg(stats.psk31)
        .arg(stats.contest_qso_points)
        .arg(stats.contest_mults)
        .arg(stats.contest_score),
      std::max(1, stats_cols)));

      const ContestTechnique technique = app_controller_get_contest_technique();
      technique_combo_->blockSignals(true);
      technique_combo_->setCurrentIndex(technique_combo_index(technique));
      technique_combo_->blockSignals(false);

      const bool dual_mode = technique != CONTEST_TECH_SO1R;
      radio2_focus_button_->setVisible(dual_mode);
      run2_toggle_button_->setVisible(dual_mode);
      swap_radios_button_->setVisible(dual_mode);
      radio2_label_->setVisible(dual_mode);

      const QString contest_exchange_label =
        QString::fromLatin1(state.contest_exchange_label ? state.contest_exchange_label : "Exchange");
      const QString contest_exchange_sent =
        QString::fromLatin1(state.contest_exchange_sent ? state.contest_exchange_sent : "");
      const QString exchange_label = contest_entry_mode
        ? QString("%1 (TX %2)").arg(contest_exchange_label,
                                     contest_exchange_sent.isEmpty() ? "-" : contest_exchange_sent)
        : "RST";
      input_rst_r1_label_->setText(exchange_label);
      input_rst_r2_label_->setText(exchange_label);

      input_r2_label_->setVisible(dual_mode);
      input_call_r2_label_->setVisible(dual_mode);
      input_call_r2_edit_->setVisible(dual_mode);
      input_rst_r2_label_->setVisible(dual_mode);
      input_rst_r2_edit_->setVisible(dual_mode);

      radio1_focus_button_->setStyleSheet(active_radio == 1
        ? "QPushButton { background: #f3d24f; color: #111; font-weight: bold; }"
        : "");
      radio2_focus_button_->setStyleSheet(active_radio == 2
        ? "QPushButton { background: #f3d24f; color: #111; font-weight: bold; }"
        : "");

      run1_toggle_button_->setText(state.radio1_run ? "R1 RUN" : "R1 S&P");
      run2_toggle_button_->setText(state.radio2_run ? "R2 RUN" : "R2 S&P");
      run1_toggle_button_->setStyleSheet(state.radio1_run
        ? "QPushButton { background: #87d37c; color: #111; font-weight: bold; }"
        : "");
      run2_toggle_button_->setStyleSheet(state.radio2_run
        ? "QPushButton { background: #87d37c; color: #111; font-weight: bold; }"
        : "");

      radio1_label_->setText(QString("R1 %1 kHz %2")
                     .arg(state.radio1_freq_khz)
                     .arg(state.radio1_mode ? state.radio1_mode : ""));
      radio2_label_->setText(QString("R2 %1 kHz %2")
                     .arg(state.radio2_freq_khz)
                     .arg(state.radio2_mode ? state.radio2_mode : ""));

    if (cty_update_in_progress) {
        function_label_->setText(
          clip_to_cols("F7 CTY update in progress... keyboard locked",
                 std::max(1, func_cols)));
      function_panel_->setStyleSheet(
          "QFrame { background: #f3d24f; color: #111; font-weight: bold; }");
    } else {
        function_label_->setText(clip_to_cols(
      "F1-F10: CW | Ctrl+F1 Help | Ctrl+F2 New Log | Ctrl+F3 Open Log | Ctrl+F4 Export | Ctrl+F5 DXCluster | Ctrl+F6 Stats | Ctrl+F7 Update CTY | Ctrl+F10 Quit | Ctrl+K CW Keyer",
          std::max(1, func_cols)));
      function_panel_->setStyleSheet(
          "QFrame { background: #0f5ea4; color: #f4f8ff; font-weight: bold; }");
    }

    refresh_log_table();
    refresh_cluster_table(false, 0);
    refresh_suggestions();
    refresh_cat_status();
    refresh_cw_keyer_status();

    log_group_->setVisible(true);
    input_panel_->setVisible(true);
    status_panel_->setVisible(true);
    dxcc_panel_->setVisible(true);
    stats_panel_->setVisible(true);
    gap_panel_->setVisible(true);
    suggestions_frame_->setVisible(call_suggestion_available);
    cluster_group_->setVisible(state.cluster_view);
  }

  QTimer *timer_ = nullptr;

  QGroupBox *log_group_ = nullptr;
  QTableWidget *log_table_ = nullptr;

  QFrame *suggestions_frame_ = nullptr;
  QListWidget *suggestions_list_ = nullptr;

  QFrame *input_panel_ = nullptr;
  QLabel *input_r1_label_ = nullptr;
  QLabel *input_call_r1_label_ = nullptr;
  QLineEdit *input_call_r1_edit_ = nullptr;
  QLabel *input_rst_r1_label_ = nullptr;
  QLineEdit *input_rst_r1_edit_ = nullptr;
  QLabel *input_comments_r1_label_ = nullptr;
  QLineEdit *input_comments_r1_edit_ = nullptr;
  QLabel *input_r2_label_ = nullptr;
  QLabel *input_call_r2_label_ = nullptr;
  QLineEdit *input_call_r2_edit_ = nullptr;
  QLabel *input_rst_r2_label_ = nullptr;
  QLineEdit *input_rst_r2_edit_ = nullptr;
  QLabel *input_comments_r2_label_ = nullptr;
  QLineEdit *input_comments_r2_edit_ = nullptr;

  QFrame *status_panel_ = nullptr;
  QLabel *status_label_ = nullptr;
  QLabel *info_label_ = nullptr;

  QFrame *dxcc_panel_ = nullptr;
  QLabel *dxcc_label_ = nullptr;

  QFrame *stats_panel_ = nullptr;
  QLabel *stats_label_ = nullptr;

  QFrame *op_panel_ = nullptr;
  QComboBox *technique_combo_ = nullptr;
  QPushButton *radio1_focus_button_ = nullptr;
  QPushButton *radio2_focus_button_ = nullptr;
  QPushButton *swap_radios_button_ = nullptr;
  QPushButton *run1_toggle_button_ = nullptr;
  QPushButton *run2_toggle_button_ = nullptr;
  QLabel *radio1_label_ = nullptr;
  QLabel *radio2_label_ = nullptr;
  QLineEdit *contest_tx_exchange_edit_ = nullptr;

  QFrame *gap_panel_ = nullptr;

  QGroupBox *cluster_group_ = nullptr;
  QTableWidget *cluster_table_ = nullptr;
  QLineEdit *cluster_search_edit_ = nullptr;
  std::array<QCheckBox *, kBandLabels.size()> cluster_band_checks_{};

  QGroupBox *cat_group_ = nullptr;
  QComboBox *cat_slot_combo_ = nullptr;
  QComboBox *cat_rig_combo_ = nullptr;
  QLineEdit *cat_device_edit_ = nullptr;
  QComboBox *cat_baud_combo_ = nullptr;
  QComboBox *cat_data_bits_combo_ = nullptr;
  QComboBox *cat_stop_bits_combo_ = nullptr;
  QComboBox *cat_parity_combo_ = nullptr;
  QComboBox *cat_handshake_combo_ = nullptr;
  QCheckBox *cat_mode_from_rig_check_ = nullptr;
  QPushButton *cat_connect_button_ = nullptr;
  QPushButton *cat_disconnect_button_ = nullptr;
  QFrame *cat_status_frame_ = nullptr;
  QLabel *cat_status_label_ = nullptr;
  QLabel *cat_slot1_status_label_ = nullptr;
  QLabel *cat_slot2_status_label_ = nullptr;

  QLineEdit *cw_device_edit_ = nullptr;
  QComboBox *cw_line_combo_ = nullptr;
  QSpinBox *cw_wpm_spin_ = nullptr;
  QPushButton *cw_connect_button_ = nullptr;
  QPushButton *cw_disconnect_button_ = nullptr;
  QLabel *cw_status_label_ = nullptr;

  QFrame *function_panel_ = nullptr;
  QLabel *function_label_ = nullptr;

  CwKeyerDialog *cw_dialog_ = nullptr;
  CwKeyMap cw_key_map_ = {};
  QString cw_feedback_;
  int cw_feedback_ticks_ = 0;

  int cell_w_ = 8;
  int cell_h_ = 16;
};

/*
 * Start the Qt frontend application.
 *
 * @param argc Command-line argument count.
 * @param argv Command-line argument vector.
 * @return Qt application exit code.
 */
int main(int argc, char **argv) {
  int qt_argc = 1;
  for (int i = 1; i < argc; i++) {
    if (std::strcmp(argv[i], "--debug") == 0) {
      app_debug_enabled = 1;
      continue;
    }

    argv[qt_argc++] = argv[i];
  }
  argv[qt_argc] = nullptr;

  QApplication app(qt_argc, argv);

  if (app_debug_enabled) {
    std::fprintf(stderr, "[debug] logger starting with --debug\n");
  }

  if (app_controller_init() != 0) {
    if (app_debug_enabled) {
      std::fprintf(stderr, "[debug] app_controller_init returned failure\n");
    }
    QMessageBox::warning(nullptr, "Logger",
                         "DXCluster startup failed. The app will continue without it.");
  }

  cat_init();

  LoggerQtWindow window;
  window.show();

  const int rc = app.exec();

  cat_disconnect_cw_keyer();
  cat_shutdown();
  app_controller_shutdown();
  return rc;
}
