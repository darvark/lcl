class QtcDialog : public QWidget {
public:
  explicit QtcDialog(const CwQtcMap *qtc_map, QWidget *parent = nullptr)
      : QWidget(parent, Qt::Dialog), qtc_map_(qtc_map) {
    setWindowTitle("QTC Exchange (Ctrl+L)");
    setAttribute(Qt::WA_DeleteOnClose, false);
    resize(720, 560);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(6);

    /* ---- Header row: receiver call and bundle info ---- */
    auto *header = new QHBoxLayout();
    header->addWidget(new QLabel("Receiver:", this));
    receiver_edit_ = new QLineEdit(this);
    receiver_edit_->setPlaceholderText("Callsign of other station");
    receiver_edit_->setMinimumWidth(140);
    header->addWidget(receiver_edit_);

    header->addSpacing(12);
    header->addWidget(new QLabel("QTC NR:", this));
    qtc_nr_edit_ = new QLineEdit(this);
    qtc_nr_edit_->setPlaceholderText("NR/TOT (np. 1/10)");
    qtc_nr_edit_->setFixedWidth(90);
    qtc_nr_edit_->setToolTip("Numer paczki QTC / liczba rekordow (np. 1/10)");
    header->addWidget(qtc_nr_edit_);

    direction_combo_ = new QComboBox(this);
    direction_combo_->addItem("Sending (I am sending QTC)", 1);
    direction_combo_->addItem("Receiving (I am receiving QTC)", 0);
    header->addWidget(direction_combo_);
    header->addStretch(1);
    root->addLayout(header);

    /* ---- QSO record table ---- */
    record_table_ = new QTableWidget(QTC_MAX_RECORDS_PER_BUNDLE, 3, this);
    record_table_->setHorizontalHeaderLabels({"Time", "Call", "Exch"});
    record_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    record_table_->verticalHeader()->setVisible(false);
    record_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    record_table_->setEditTriggers(QAbstractItemView::AllEditTriggers);
    for (int i = 0; i < QTC_MAX_RECORDS_PER_BUNDLE; i++) {
      for (int j = 0; j < 3; j++)
        record_table_->setItem(i, j, new QTableWidgetItem(""));
    }
    root->addWidget(record_table_, 1);

    /* ---- CW sending buttons ---- */
    auto *cw_group = new QGroupBox("CW Send", this);
    auto *cw_layout = new QHBoxLayout(cw_group);
    send_preamble_btn_ = new QPushButton("Send Preamble", cw_group);
    send_preamble_btn_->setToolTip("Send QTC NR/COUNT announcement via CW");
    send_record_btn_   = new QPushButton("Send Record", cw_group);
    send_record_btn_->setToolTip("Send selected record via CW");
    send_confirm_btn_  = new QPushButton("Send QSL", cw_group);
    send_confirm_btn_->setToolTip("Send confirmation after receiving a bundle");
    send_request_btn_  = new QPushButton("Send QRV", cw_group);
    send_request_btn_->setToolTip("Request QTC from the other station");
    cw_layout->addWidget(send_preamble_btn_);
    cw_layout->addWidget(send_record_btn_);
    cw_layout->addWidget(send_confirm_btn_);
    cw_layout->addWidget(send_request_btn_);
    root->addWidget(cw_group);

    /* ---- Pre-fill and action buttons ---- */
    auto *btn_row = new QHBoxLayout();
    prefill_btn_ = new QPushButton("Pre-fill from Log", this);
    prefill_btn_->setToolTip(
        "Populate table with recent QSOs not yet sent as QTC");
    save_btn_ = new QPushButton("Save QTC Bundle", this);
    save_btn_->setDefault(true);
    close_btn_ = new QPushButton("Close", this);
    btn_row->addWidget(prefill_btn_);
    btn_row->addStretch(1);
    btn_row->addWidget(save_btn_);
    btn_row->addWidget(close_btn_);
    root->addLayout(btn_row);

    status_label_ = new QLabel(this);
    root->addWidget(status_label_);

    /* ---- Connections ---- */
    connect(prefill_btn_,      &QPushButton::clicked, this,
            [this]() { on_prefill(); });
    connect(save_btn_,         &QPushButton::clicked, this,
            [this]() { on_save(); });
    connect(close_btn_,        &QPushButton::clicked, this,
          [this]() {
            hide();
            if (parentWidget()) {
        parentWidget()->activateWindow();
        parentWidget()->setFocus();
            }
          });
    connect(send_preamble_btn_, &QPushButton::clicked, this,
            [this]() { on_send_preamble(); });
    connect(send_record_btn_,   &QPushButton::clicked, this,
            [this]() { on_send_selected_record(); });
    connect(send_confirm_btn_,  &QPushButton::clicked, this,
            [this]() { cw_send(qtc_map_ ? qtc_map_->confirm : "QSL"); });
    connect(send_request_btn_,  &QPushButton::clicked, this,
            [this]() { cw_send(qtc_map_ ? qtc_map_->request : "QRV"); });
  }

  void showAndFocus() {
    /* Refresh bundle number to the next available one. */
    char receiver[32] = {0};
    app_controller_qtc_get_receiver_call(receiver, sizeof(receiver));
    if (receiver[0])
      receiver_edit_->setText(QString::fromLatin1(receiver));

    const int next_nr =
        qtc_next_bundle_nr(config_effective_operator_call(), receiver);
    qtc_nr_edit_->setText(QString("%1/10").arg(next_nr));

    status_label_->clear();
    show();
    raise();
    activateWindow();
    receiver_edit_->setFocus();
  }

protected:
  void keyPressEvent(QKeyEvent *event) override {
    if (event->key() == Qt::Key_Escape) {
      hide();
      if (parentWidget()) {
        parentWidget()->activateWindow();
        parentWidget()->setFocus();
      }
      event->accept();
      return;
    }
    QWidget::keyPressEvent(event);
  }

private:
  /* Send text via CW keyer (same fallback chain as CwKeyerDialog). */
  void cw_send(const char *text) {
    if (!text || !text[0])
      return;

    const QByteArray bytes = QByteArray(text).toUpper();

    if (cat_is_cw_keyer_connected()) {
      cat_cw_send(bytes.constData());
      return;
    }

    cat_send_morse_slot(CAT_SLOT_A, bytes.constData());
  }

  /* Build expanded CW text from a QTC template and current dialog state. */
  void cw_send_expanded(const char *tpl, const char *qtc_time,
                        const char *qtc_call, const char *qtc_exch) {
    if (!tpl || !tpl[0])
      return;

    char expanded[256] = {0};
    cw_qtc_expand(tpl,
                  config_effective_operator_call(),
                  receiver_edit_->text().toLatin1().constData(),
                  qtc_nr_parts().first,
                  qtc_nr_parts().second > 0 ? qtc_nr_parts().second : valid_record_count(),
                  qtc_time, qtc_call, qtc_exch,
                  expanded, sizeof(expanded));
    cw_send(expanded);
  }

  /* Count table rows that have a non-empty callsign column. */
  int valid_record_count() const {
    int n = 0;
    for (int i = 0; i < QTC_MAX_RECORDS_PER_BUNDLE; i++) {
      const QTableWidgetItem *item = record_table_->item(i, 1);  /* col 1 = Call */
      if (item && !item->text().trimmed().isEmpty())
        n++;
    }
    return n;
  }

  /* Parse "XX/YY" from qtc_nr_edit_ — returns {bundle_nr, record_count}.
   * If only a plain number is entered it is used as bundle_nr with count=0. */
  std::pair<int, int> qtc_nr_parts() const {
    const QString text = qtc_nr_edit_ ? qtc_nr_edit_->text().trimmed() : "";
    const int slash = text.indexOf('/');
    if (slash >= 0) {
      const int nr  = text.left(slash).trimmed().toInt();
      const int tot = text.mid(slash + 1).trimmed().toInt();
      return {nr > 0 ? nr : 1, tot > 0 ? tot : 0};
    }
    const int nr = text.toInt();
    return {nr > 0 ? nr : 1, 0};
  }

  void on_send_preamble() {
    if (!qtc_map_) return;
    cw_send_expanded(qtc_map_->preamble, "", "", "");
  }

  void on_send_selected_record() {
    if (!qtc_map_) return;

    const int row = record_table_->currentRow();
    if (row < 0 || row >= QTC_MAX_RECORDS_PER_BUNDLE) {
      status_label_->setText("Select a record row first");
      return;
    }

    auto cell = [&](int col) -> QByteArray {
      const QTableWidgetItem *it = record_table_->item(row, col);
      return it ? it->text().trimmed().toLatin1() : QByteArray();
    };

    cw_send_expanded(qtc_map_->record,
                     cell(0).constData(),   /* time */
                     cell(1).constData(),   /* call */
                     cell(2).constData());  /* exch */
  }

  void on_prefill() {
    QTCRecord records[QTC_MAX_RECORDS_PER_BUNDLE];
    const int n = app_controller_qtc_get_sendable(records,
                                                   QTC_MAX_RECORDS_PER_BUNDLE);
    for (int i = 0; i < QTC_MAX_RECORDS_PER_BUNDLE; i++) {
      auto set_cell = [&](int col, const char *text) {
        QTableWidgetItem *it = record_table_->item(i, col);
        if (!it) {
          it = new QTableWidgetItem();
          record_table_->setItem(i, col, it);
        }
        it->setText(i < n ? QString::fromLatin1(text) : "");
      };

      set_cell(0, i < n ? records[i].time : "");
      set_cell(1, i < n ? records[i].call : "");
      set_cell(2, i < n ? records[i].exch : "");
    }

    status_label_->setText(
        QString("Pre-filled %1 record(s) from logbook").arg(n));
  }

  void on_save() {
    const QString recv_call = receiver_edit_->text().trimmed();
    if (recv_call.isEmpty()) {
      status_label_->setText("Enter receiver callsign");
      return;
    }

    QTCBundle bundle;
    std::memset(&bundle, 0, sizeof(bundle));

    bundle.bundle_nr   = qtc_nr_parts().first;
    bundle.sent        = direction_combo_->currentData().toInt();
    bundle.record_count = 0;

    const QByteArray peer = recv_call.toUpper().toLatin1();
    if (bundle.sent) {
      std::strncpy(bundle.sender_call, config.station_call,
                   sizeof(bundle.sender_call) - 1);
      std::strncpy(bundle.receiver_call, peer.constData(),
                   sizeof(bundle.receiver_call) - 1);
    } else {
      std::strncpy(bundle.sender_call, peer.constData(),
                   sizeof(bundle.sender_call) - 1);
      std::strncpy(bundle.receiver_call, config.station_call,
                   sizeof(bundle.receiver_call) - 1);
    }

    if (!bundle.sender_call[0]) {
      status_label_->setText("Sender callsign is empty (set station call)");
      return;
    }

    if (!bundle.receiver_call[0]) {
      status_label_->setText("Peer (receiver) callsign is empty");
      return;
    }

    for (int i = 0; i < QTC_MAX_RECORDS_PER_BUNDLE; i++) {
      auto cell_text = [&](int col) -> QByteArray {
        const QTableWidgetItem *it = record_table_->item(i, col);
        return it ? it->text().trimmed().toUpper().toLatin1() : QByteArray();
      };

      const QByteArray call = cell_text(1);  /* col 1 = Call */
      if (call.isEmpty())
        continue;

      QTCRecord &r = bundle.records[bundle.record_count++];
      std::memset(&r, 0, sizeof(r));
      /* date is not entered manually; left empty for received QTC */
      std::strncpy(r.time, cell_text(0).constData(), sizeof(r.time) - 1);
      std::strncpy(r.call, call.constData(),         sizeof(r.call) - 1);
      std::strncpy(r.exch, cell_text(2).constData(), sizeof(r.exch) - 1);
    }

    if (bundle.record_count == 0) {
      status_label_->setText("No records to save");
      return;
    }

    char status[128] = {0};
    if (app_controller_qtc_submit(&bundle, status, sizeof(status)) == 0) {
      status_label_->setText(QString::fromLatin1(status));
      /* Auto-increment the bundle number part (XX) keeping YY as record count. */
      const auto parts = qtc_nr_parts();
      qtc_nr_edit_->setText(
          QString("%1/%2").arg(parts.first + 1).arg(bundle.record_count));
      /* Clear the table for the next bundle. */
      for (int i = 0; i < QTC_MAX_RECORDS_PER_BUNDLE; i++)
        for (int j = 0; j < 3; j++) {
          QTableWidgetItem *it = record_table_->item(i, j);
          if (it) it->setText("");
        }
      /* Close the window — bundle has been saved, user returns to logging. */
      hide();
      if (parentWidget()) {
        parentWidget()->activateWindow();
        parentWidget()->setFocus();
      }
    } else {
      status_label_->setText(
          QString("Error: %1").arg(QString::fromLatin1(status)));
    }
  }

  const CwQtcMap *qtc_map_;
  QLineEdit      *receiver_edit_   = nullptr;
  QLineEdit      *qtc_nr_edit_     = nullptr;
  QComboBox      *direction_combo_ = nullptr;
  QTableWidget   *record_table_    = nullptr;
  QPushButton    *prefill_btn_     = nullptr;
  QPushButton    *save_btn_        = nullptr;
  QPushButton    *close_btn_       = nullptr;
  QPushButton    *send_preamble_btn_ = nullptr;
  QPushButton    *send_record_btn_   = nullptr;
  QPushButton    *send_confirm_btn_  = nullptr;
  QPushButton    *send_request_btn_  = nullptr;
  QLabel         *status_label_    = nullptr;
};

/* ------------------------------------------------------------------ */
/* QTC History Dialog                                                   */
/* ------------------------------------------------------------------ */

class QtcHistoryDialog : public QWidget {
public:
  explicit QtcHistoryDialog(QWidget *parent = nullptr)
      : QWidget(parent, Qt::Dialog) {
    setWindowTitle("QTC History (Ctrl+Shift+L)");
    setAttribute(Qt::WA_DeleteOnClose, false);
    resize(860, 600);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(6);

    auto *top = new QHBoxLayout();
    top->addWidget(new QLabel("Filter:", this));
    filter_combo_ = new QComboBox(this);
    filter_combo_->addItem("All", -1);
    filter_combo_->addItem("Sent", 1);
    filter_combo_->addItem("Received", 0);
    top->addWidget(filter_combo_);
    top->addStretch(1);
    refresh_btn_ = new QPushButton("Refresh", this);
    close_btn_ = new QPushButton("Close", this);
    top->addWidget(refresh_btn_);
    top->addWidget(close_btn_);
    root->addLayout(top);

    bundle_table_ = new QTableWidget(0, 6, this);
    bundle_table_->setHorizontalHeaderLabels(
      {"Dir", "NR/COUNT", "Peer (Receiver)", "Sender", "First Date", "First Time"});
    bundle_table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    bundle_table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    bundle_table_->verticalHeader()->setVisible(false);
    bundle_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    bundle_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    bundle_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    root->addWidget(bundle_table_, 1);

    record_table_ = new QTableWidget(0, 4, this);
    record_table_->setHorizontalHeaderLabels({"Time", "Call", "Exch", "Date"});
    record_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    record_table_->verticalHeader()->setVisible(false);
    record_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    record_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    root->addWidget(record_table_, 1);

    status_label_ = new QLabel(this);
    root->addWidget(status_label_);

    connect(filter_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { reload(); });
    connect(refresh_btn_, &QPushButton::clicked, this,
            [this]() { reload(); });
    connect(close_btn_, &QPushButton::clicked, this,
          [this]() {
            hide();
            if (parentWidget()) {
        parentWidget()->activateWindow();
        parentWidget()->setFocus();
            }
          });
    connect(bundle_table_, &QTableWidget::currentCellChanged, this,
            [this](int row, int, int, int) { update_record_table_for_row(row); });
  }

  void showAndFocus() {
    reload();
    show();
    raise();
    activateWindow();
    bundle_table_->setFocus();
  }

protected:
  void keyPressEvent(QKeyEvent *event) override {
    if (event->key() == Qt::Key_Escape) {
      hide();
      if (parentWidget()) {
        parentWidget()->activateWindow();
        parentWidget()->setFocus();
      }
      event->accept();
      return;
    }
    QWidget::keyPressEvent(event);
  }

private:
  void set_cell(QTableWidget *table, int row, int col, const QString &text,
                const QString &tip = QString()) {
    QTableWidgetItem *item = table->item(row, col);
    if (!item) {
      item = new QTableWidgetItem();
      table->setItem(row, col, item);
    }
    item->setText(text);
    item->setToolTip(tip.isEmpty() ? text : tip);
    item->setTextAlignment(Qt::AlignCenter | Qt::AlignVCenter);
  }

  void reload() {
    visible_bundle_indices_.clear();

    const int filter = filter_combo_ ? filter_combo_->currentData().toInt() : -1;
    int shown_records = 0;

    for (int i = 0; i < qtc_bundle_count; i++) {
      const QTCBundle &b = qtc_bundles[i];
      if (filter != -1 && (b.sent ? 1 : 0) != filter)
        continue;
      visible_bundle_indices_.push_back(i);
      shown_records += b.record_count;
    }

    bundle_table_->setRowCount((int)visible_bundle_indices_.size());
    for (int row = 0; row < (int)visible_bundle_indices_.size(); row++) {
      const QTCBundle &b = qtc_bundles[visible_bundle_indices_[(size_t)row]];
      const QString dir = b.sent ? "SENT" : "RCVD";
      const QString peer = QString::fromLatin1(b.receiver_call);
      const QString nr_count = QString("%1/%2").arg(b.bundle_nr).arg(b.record_count);
      const QString first_date = b.record_count > 0
                                     ? QString::fromLatin1(b.records[0].date)
                                     : QString();
      const QString first_time = b.record_count > 0
                                     ? QString::fromLatin1(b.records[0].time)
                                     : QString();

      set_cell(bundle_table_, row, 0, dir);
      set_cell(bundle_table_, row, 1, nr_count);
      set_cell(bundle_table_, row, 2, peer);
      set_cell(bundle_table_, row, 3, QString::fromLatin1(b.sender_call));
      set_cell(bundle_table_, row, 4, first_date);
      set_cell(bundle_table_, row, 5, first_time);
    }

    if (!visible_bundle_indices_.empty()) {
      bundle_table_->setCurrentCell(0, 0);
      update_record_table_for_row(0);
    } else {
      record_table_->setRowCount(0);
    }

    status_label_->setText(
        QString("Bundles: %1, Records: %2")
            .arg((int)visible_bundle_indices_.size())
            .arg(shown_records));
  }

  void update_record_table_for_row(int row) {
    if (row < 0 || row >= (int)visible_bundle_indices_.size()) {
      record_table_->setRowCount(0);
      return;
    }

    const QTCBundle &b = qtc_bundles[visible_bundle_indices_[(size_t)row]];
    record_table_->setRowCount(b.record_count);

    for (int i = 0; i < b.record_count; i++) {
      const QTCRecord &r = b.records[i];
      set_cell(record_table_, i, 0, QString::fromLatin1(r.time));
      set_cell(record_table_, i, 1, QString::fromLatin1(r.call));
      set_cell(record_table_, i, 2, QString::fromLatin1(r.exch));
      set_cell(record_table_, i, 3, QString::fromLatin1(r.date));
    }
  }

  QComboBox *filter_combo_ = nullptr;
  QPushButton *refresh_btn_ = nullptr;
  QPushButton *close_btn_ = nullptr;
  QTableWidget *bundle_table_ = nullptr;
  QTableWidget *record_table_ = nullptr;
  QLabel *status_label_ = nullptr;
  std::vector<int> visible_bundle_indices_;
};

class DocumentationDialog : public QWidget {
public:
  explicit DocumentationDialog(QWidget *parent = nullptr)
      : QWidget(parent, Qt::Dialog) {
    setWindowTitle("Documentation (Ctrl+F1)");
    setAttribute(Qt::WA_DeleteOnClose, false);
    resize(960, 640);

        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(8, 8, 8, 8);
        layout->setSpacing(8);

        auto *left_panel = new QWidget(this);
        auto *left_layout = new QVBoxLayout(left_panel);
        left_layout->setContentsMargins(0, 0, 0, 0);
        left_layout->setSpacing(4);
        left_layout->addWidget(new QLabel("Files:", left_panel));
        file_filter_edit_ = new QLineEdit(left_panel);
        file_filter_edit_->setPlaceholderText("Filter files...");
        left_layout->addWidget(file_filter_edit_);

        doc_list_ = new QListWidget(left_panel);
        doc_list_->setMinimumWidth(260);
        left_layout->addWidget(doc_list_, 1);
        layout->addWidget(left_panel, 0);

        auto *right_panel = new QWidget(this);
        auto *right_layout = new QVBoxLayout(right_panel);
        right_layout->setContentsMargins(0, 0, 0, 0);
        right_layout->setSpacing(4);

        auto *search_row = new QHBoxLayout();
        search_row->addWidget(new QLabel("Search:", right_panel));
        search_edit_ = new QLineEdit(right_panel);
        search_edit_->setPlaceholderText("Find in current file...");
        search_row->addWidget(search_edit_, 1);
        match_case_check_ = new QCheckBox("Match case", right_panel);
        search_row->addWidget(match_case_check_);
        render_markdown_check_ = new QCheckBox("Render Markdown", right_panel);
        render_markdown_check_->setChecked(true);
        search_row->addWidget(render_markdown_check_);
        find_prev_button_ = new QPushButton("Prev", right_panel);
        find_next_button_ = new QPushButton("Next", right_panel);
        search_row->addWidget(find_prev_button_);
        search_row->addWidget(find_next_button_);
        search_status_label_ = new QLabel(right_panel);
        search_row->addWidget(search_status_label_);
        right_layout->addLayout(search_row);

        doc_view_ = new QTextBrowser(right_panel);
        doc_view_->setReadOnly(true);
        doc_view_->setOpenExternalLinks(false);
        doc_view_->setLineWrapMode(QTextEdit::NoWrap);
        right_layout->addWidget(doc_view_, 1);
        layout->addWidget(right_panel, 1);

    connect(doc_list_, &QListWidget::currentRowChanged, this,
            [this](int row) { load_selected(row); });
        connect(file_filter_edit_, &QLineEdit::textChanged, this,
          [this](const QString &text) { apply_file_filter(text); });
        connect(search_edit_, &QLineEdit::textChanged, this,
          [this](const QString &text) { find_from_top(text); });
        connect(search_edit_, &QLineEdit::returnPressed, this,
          [this]() { find_next(); });
        connect(match_case_check_, &QCheckBox::toggled, this,
          [this](bool) { find_from_top(search_edit_ ? search_edit_->text() : QString()); });
        connect(render_markdown_check_, &QCheckBox::toggled, this,
          [this](bool) {
            apply_document_view_mode();
            find_from_top(search_edit_ ? search_edit_->text() : QString());
          });
        connect(find_next_button_, &QPushButton::clicked, this,
          [this]() { find_next(); });
        connect(find_prev_button_, &QPushButton::clicked, this,
          [this]() { find_prev(); });

    reload_doc_index();
  }

  void showAndFocus() {
    reload_doc_index();
    show();
    raise();
    activateWindow();
    doc_list_->setFocus();
  }

private:
  void reload_doc_index() {
    if (doc_list_ && doc_list_->currentRow() >= 0 &&
        doc_list_->currentRow() < (int)visible_doc_indices_.size()) {
      const int idx = visible_doc_indices_[(size_t)doc_list_->currentRow()];
      if (idx >= 0 && idx < (int)doc_paths_.size())
        current_doc_path_ = doc_paths_[(size_t)idx];
    }

    doc_paths_.clear();
    doc_labels_.clear();
    visible_doc_indices_.clear();

    QSet<QString> seen;

    auto add_if_exists = [this, &seen](const QString &label,
                                       const QString &path) {
      QFileInfo info(path);
      if (!info.exists() || !info.isFile() || !info.isReadable())
        return;

      const QString canonical = info.canonicalFilePath();
      if (canonical.isEmpty() || seen.contains(canonical))
        return;

      seen.insert(canonical);
      doc_labels_.push_back(label);
      doc_paths_.push_back(canonical);
    };

    const QString app_dir = QApplication::applicationDirPath();
    const QStringList roots = {
        QDir::currentPath(), app_dir, app_dir + "/..", app_dir + "/../.."};

    for (const QString &root : roots) {
      QDir root_dir(root);
      add_if_exists("README.md", root_dir.filePath("README.md"));

      QDir docs_dir(root_dir.filePath("docs"));
      if (!docs_dir.exists())
        continue;

      const QStringList names = docs_dir.entryList({"*.md"}, QDir::Files,
                                                   QDir::Name);
      for (const QString &name : names)
        add_if_exists(QString("docs/%1").arg(name), docs_dir.filePath(name));
    }

    apply_file_filter(file_filter_edit_ ? file_filter_edit_->text() : QString());
    if (!current_doc_path_.isEmpty())
      select_doc_path(current_doc_path_);
  }

  void apply_file_filter(const QString &filter_text) {
    doc_list_->clear();
    visible_doc_indices_.clear();

    const QString needle = filter_text.trimmed();
    for (int i = 0; i < (int)doc_labels_.size(); i++) {
      if (!needle.isEmpty() &&
          !doc_labels_[(size_t)i].contains(needle, Qt::CaseInsensitive) &&
          !doc_paths_[(size_t)i].contains(needle, Qt::CaseInsensitive)) {
        continue;
      }

      visible_doc_indices_.push_back(i);
      doc_list_->addItem(doc_labels_[(size_t)i]);
    }

    if (visible_doc_indices_.empty()) {
      current_doc_text_.clear();
      doc_view_->setPlainText("No files match current filter.");
      doc_view_->setExtraSelections({});
      search_status_label_->setText("-");
      return;
    }

    if (!select_doc_path(current_doc_path_)) {
      doc_list_->setCurrentRow(0);
      load_selected(0);
    }
  }

  bool select_doc_path(const QString &path) {
    if (path.isEmpty())
      return false;

    for (int row = 0; row < (int)visible_doc_indices_.size(); row++) {
      const int idx = visible_doc_indices_[(size_t)row];
      if (idx >= 0 && idx < (int)doc_paths_.size() &&
          doc_paths_[(size_t)idx] == path) {
        doc_list_->setCurrentRow(row);
        load_selected(row);
        return true;
      }
    }

    return false;
  }

  void load_selected(int row) {
    if (row < 0 || row >= (int)visible_doc_indices_.size()) {
      doc_view_->clear();
      return;
    }

    const int doc_idx = visible_doc_indices_[(size_t)row];
    if (doc_idx < 0 || doc_idx >= (int)doc_paths_.size()) {
      doc_view_->clear();
      return;
    }

    const QString &selected_path = doc_paths_[(size_t)doc_idx];
    current_doc_path_ = selected_path;

    QFile file(selected_path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
      current_doc_text_.clear();
      doc_view_->setPlainText(
          QString("Cannot open file: %1").arg(selected_path));
      doc_view_->setExtraSelections({});
      return;
    }

    const QByteArray content = file.readAll();
    current_doc_text_ = QString::fromUtf8(content);
    apply_document_view_mode();
    find_from_top(search_edit_ ? search_edit_->text() : QString());
  }

  void apply_document_view_mode() {
    const bool render_markdown =
        render_markdown_check_ && render_markdown_check_->isChecked();

    if (render_markdown) {
      doc_view_->setMarkdown(current_doc_text_);
      doc_view_->setLineWrapMode(QTextEdit::WidgetWidth);
    } else {
      doc_view_->setPlainText(current_doc_text_);
      doc_view_->setLineWrapMode(QTextEdit::NoWrap);
    }
  }

  QTextDocument::FindFlags find_flags(bool backward = false) const {
    QTextDocument::FindFlags flags;
    if (backward)
      flags |= QTextDocument::FindBackward;
    if (match_case_check_ && match_case_check_->isChecked())
      flags |= QTextDocument::FindCaseSensitively;
    return flags;
  }

  void highlight_all_matches(const QString &text) {
    QList<QTextEdit::ExtraSelection> selections;

    if (!text.isEmpty()) {
      QTextCursor cursor(doc_view_->document());
      while (true) {
        cursor = doc_view_->document()->find(text, cursor, find_flags(false));
        if (cursor.isNull())
          break;

        QTextEdit::ExtraSelection sel;
        sel.cursor = cursor;
        sel.format.setBackground(QColor(255, 239, 140));
        sel.format.setForeground(QColor(20, 20, 20));
        selections.push_back(sel);
      }

      QTextCursor active = doc_view_->textCursor();
      if (active.hasSelection()) {
        QTextEdit::ExtraSelection active_sel;
        active_sel.cursor = active;
        active_sel.format.setBackground(QColor(245, 183, 77));
        active_sel.format.setForeground(QColor(20, 20, 20));
        selections.push_back(active_sel);
      }
    }

    doc_view_->setExtraSelections(selections);
    match_count_ = (int)selections.size();
    if (doc_view_->textCursor().hasSelection() && match_count_ > 0)
      match_count_ -= 1;
  }

  void find_from_top(const QString &needle) {
    const QString text = needle;
    if (text.isEmpty()) {
      doc_view_->setExtraSelections({});
      search_status_label_->setText("-");
      return;
    }

    QTextCursor cursor = doc_view_->textCursor();
    cursor.movePosition(QTextCursor::Start);
    doc_view_->setTextCursor(cursor);

    const bool found = doc_view_->find(text, find_flags(false));
    highlight_all_matches(text);
    search_status_label_->setText(
        found ? QString("Found (%1)").arg(match_count_)
              : "Not found");
  }

  void find_next() {
    const QString text = search_edit_ ? search_edit_->text() : QString();
    if (text.isEmpty()) {
      search_status_label_->setText("-");
      return;
    }

    if (doc_view_->find(text, find_flags(false))) {
      highlight_all_matches(text);
      search_status_label_->setText(QString("Found (%1)").arg(match_count_));
      return;
    }

    QTextCursor cursor = doc_view_->textCursor();
    cursor.movePosition(QTextCursor::Start);
    doc_view_->setTextCursor(cursor);
    const bool wrapped = doc_view_->find(text, find_flags(false));
    highlight_all_matches(text);
    search_status_label_->setText(
      wrapped ? QString("Wrapped (%1)").arg(match_count_) : "Not found");
  }

  void find_prev() {
    const QString text = search_edit_ ? search_edit_->text() : QString();
    if (text.isEmpty()) {
      search_status_label_->setText("-");
      return;
    }

    if (doc_view_->find(text, find_flags(true))) {
      highlight_all_matches(text);
      search_status_label_->setText(QString("Found (%1)").arg(match_count_));
      return;
    }

    QTextCursor cursor = doc_view_->textCursor();
    cursor.movePosition(QTextCursor::End);
    doc_view_->setTextCursor(cursor);
    const bool wrapped = doc_view_->find(text, find_flags(true));
    highlight_all_matches(text);
    search_status_label_->setText(
        wrapped ? QString("Wrapped (%1)").arg(match_count_) : "Not found");
  }

  QLineEdit *file_filter_edit_ = nullptr;
  QListWidget *doc_list_ = nullptr;
  QLineEdit *search_edit_ = nullptr;
  QCheckBox *match_case_check_ = nullptr;
  QCheckBox *render_markdown_check_ = nullptr;
  QPushButton *find_prev_button_ = nullptr;
  QPushButton *find_next_button_ = nullptr;
  QLabel *search_status_label_ = nullptr;
  QTextBrowser *doc_view_ = nullptr;
  std::vector<QString> doc_labels_;
  std::vector<QString> doc_paths_;
  std::vector<int> visible_doc_indices_;
  QString current_doc_path_;
  QString current_doc_text_;
  int match_count_ = 0;
};

class ClickableStatusLabel : public QLabel {
public:
  explicit ClickableStatusLabel(const QString &text = QString(), QWidget *parent = nullptr)
      : QLabel(text, parent) {
    setCursor(Qt::PointingHandCursor);
  }

  void set_on_click(std::function<void()> on_click) {
    on_click_ = std::move(on_click);
  }

protected:
  void mousePressEvent(QMouseEvent *event) override {
    if (event && event->button() == Qt::LeftButton && on_click_)
      on_click_();
    QLabel::mousePressEvent(event);
  }

private:
  std::function<void()> on_click_;
};

