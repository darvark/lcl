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

class CwMessagesConfigDialog : public QWidget {
public:
  explicit CwMessagesConfigDialog(QWidget *parent = nullptr)
      : QWidget(parent, Qt::Dialog) {
    setWindowTitle("CW Messages F1-F10 (Ctrl+Shift+K)");
    setAttribute(Qt::WA_DeleteOnClose, false);
    resize(900, 520);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    auto *hint = new QLabel(
        "Edit CW templates for RUN and S&P. Placeholders: {MYCALL}, {RST}, {EXCH}, {HISCALL}/{CALL}",
        this);
    hint->setWordWrap(true);
    root->addWidget(hint);

    auto *grid = new QGridLayout();
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(6);

    grid->addWidget(new QLabel("Key", this), 0, 0);
    grid->addWidget(new QLabel("RUN", this), 0, 1);
    grid->addWidget(new QLabel("S&P", this), 0, 2);

    for (int i = 0; i < CW_KEY_COUNT; i++) {
      auto *key_label = new QLabel(QString("F%1").arg(i + 1), this);
      run_edits_[i] = new QLineEdit(this);
      sp_edits_[i] = new QLineEdit(this);
      run_edits_[i]->setPlaceholderText("RUN message");
      sp_edits_[i]->setPlaceholderText("S&P message");
      grid->addWidget(key_label, i + 1, 0);
      grid->addWidget(run_edits_[i], i + 1, 1);
      grid->addWidget(sp_edits_[i], i + 1, 2);
    }

    root->addLayout(grid);

    status_label_ = new QLabel(this);
    root->addWidget(status_label_);

    auto *buttons = new QHBoxLayout();
    auto *reload_btn = new QPushButton("Reload", this);
    auto *save_btn = new QPushButton("Save", this);
    auto *close_btn = new QPushButton("Close", this);
    save_btn->setDefault(true);
    buttons->addWidget(reload_btn);
    buttons->addStretch(1);
    buttons->addWidget(save_btn);
    buttons->addWidget(close_btn);
    root->addLayout(buttons);

    connect(reload_btn, &QPushButton::clicked, this, [this]() {
      if (reload_handler_)
        reload_handler_();
      if (load_handler_)
        set_map(load_handler_());
      status_label_->setText("Reloaded from cw_keys.ini");
    });

    connect(save_btn, &QPushButton::clicked, this, [this]() {
      if (!save_handler_)
        return;
      const bool ok = save_handler_(current_map());
      status_label_->setText(ok ? "Saved to cw_keys.ini" : "Save failed (check file permissions)");
    });

    connect(close_btn, &QPushButton::clicked, this, [this]() {
      hide();
      if (parentWidget()) {
        parentWidget()->activateWindow();
        parentWidget()->setFocus();
      }
    });
  }

  void set_map(const CwKeyMap &map) {
    for (int i = 0; i < CW_KEY_COUNT; i++) {
      run_edits_[i]->setText(QString::fromLatin1(map.run[i]));
      sp_edits_[i]->setText(QString::fromLatin1(map.sp[i]));
    }
  }

  CwKeyMap current_map() const {
    CwKeyMap out = {};
    for (int i = 0; i < CW_KEY_COUNT; i++) {
      const QByteArray run = run_edits_[i]->text().toLatin1();
      const QByteArray sp = sp_edits_[i]->text().toLatin1();
      std::snprintf(out.run[i], sizeof(out.run[i]), "%s", run.constData());
      std::snprintf(out.sp[i], sizeof(out.sp[i]), "%s", sp.constData());
    }
    return out;
  }

  void set_save_handler(std::function<bool(const CwKeyMap &)> handler) {
    save_handler_ = std::move(handler);
  }

  void set_load_handler(std::function<CwKeyMap()> handler) {
    load_handler_ = std::move(handler);
  }

  void set_reload_handler(std::function<void()> handler) {
    reload_handler_ = std::move(handler);
  }

  void showAndFocus() {
    show();
    raise();
    activateWindow();
    if (run_edits_[0])
      run_edits_[0]->setFocus();
  }

private:
  std::array<QLineEdit *, CW_KEY_COUNT> run_edits_{};
  std::array<QLineEdit *, CW_KEY_COUNT> sp_edits_{};
  QLabel *status_label_ = nullptr;
  std::function<bool(const CwKeyMap &)> save_handler_;
  std::function<CwKeyMap()> load_handler_;
  std::function<void()> reload_handler_;
};

/* ------------------------------------------------------------------ */
/* QTC Exchange Dialog (Ctrl+L)                                         */
/* ------------------------------------------------------------------ */

