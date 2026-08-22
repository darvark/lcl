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

    bandmap_group_ = new QGroupBox("Bandmap", central);
    bandmap_group_->setObjectName("bandmapGroup");
    auto *bandmap_layout = new QVBoxLayout(bandmap_group_);
    bandmap_layout->setContentsMargins(6, 24, 6, 6);
    bandmap_layout->setSpacing(4);
    bandmap_label_ = new QLabel("Bandmap (current band)", bandmap_group_);
    bandmap_layout->addWidget(bandmap_label_);
    bandmap_list_ = new QListWidget(bandmap_group_);
    bandmap_list_->setSelectionMode(QAbstractItemView::SingleSelection);
    bandmap_list_->setFocusPolicy(Qt::NoFocus);
    bandmap_list_->setUniformItemSizes(true);
    bandmap_layout->addWidget(bandmap_list_, 1);
    bandmap_group_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    bandmap_group_->setMinimumWidth(170);

    connect(bandmap_list_, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *item) {
              if (!item)
                return;
              tune_to_bandmap_row(bandmap_list_->row(item), 1);
            });

    bandmap_selected_row_ = -1;
    bandmap2_selected_row_ = -1;

        auto *bandmap_next_shortcut = new QShortcut(QKeySequence("Ctrl+Down"), this);
        bandmap_next_shortcut->setContext(Qt::ApplicationShortcut);
        connect(bandmap_next_shortcut, &QShortcut::activated, this,
          [this]() { navigate_bandmap(+1); });

        auto *bandmap_prev_shortcut = new QShortcut(QKeySequence("Ctrl+Up"), this);
        bandmap_prev_shortcut->setContext(Qt::ApplicationShortcut);
        connect(bandmap_prev_shortcut, &QShortcut::activated, this,
          [this]() { navigate_bandmap(-1); });

    auto *operator_shortcut = new QShortcut(QKeySequence("Ctrl+O"), this);
    operator_shortcut->setContext(Qt::ApplicationShortcut);
    connect(operator_shortcut, &QShortcut::activated, this,
            [this]() { prompt_operator_call(); });

    auto *left_column = new QWidget(central);
    left_column->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    left_column->setMinimumWidth(420);
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
    suggestions_frame_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    suggestions_frame_->setMinimumSize(180, 130);
    auto *suggest_layout = new QVBoxLayout(suggestions_frame_);
    suggest_layout->setContentsMargins(6, 6, 6, 6);
    suggest_layout->setSpacing(2);
    auto *suggest_title = new QLabel("Suggestions / SCP", suggestions_frame_);
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
    input_panel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *input_layout = new QGridLayout(input_panel_);
    input_layout->setContentsMargins(8, 4, 8, 4);
    input_layout->setHorizontalSpacing(6);
    input_layout->setVerticalSpacing(4);

    input_r1_label_ = new QLabel("R1", input_panel_);
    input_call_r1_label_ = new QLabel("Call", input_panel_);
    input_call_r1_edit_ = new QLineEdit(input_panel_);
    input_rst_r1_label_ = new QLabel("EXCH", input_panel_);
    input_rst_r1_edit_ = new QLineEdit(input_panel_);
    input_comments_r1_label_ = new QLabel("Comments", input_panel_);
    input_comments_r1_edit_ = new QLineEdit(input_panel_);

    input_r2_label_ = new QLabel("R2", input_panel_);
    input_call_r2_label_ = new QLabel("Call", input_panel_);
    input_call_r2_edit_ = new QLineEdit(input_panel_);
    input_rst_r2_label_ = new QLabel("EXCH", input_panel_);
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

    {
      QFont input_font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
      input_font.setPointSize(15);
      input_call_r1_edit_->setFont(input_font);
      input_rst_r1_edit_->setFont(input_font);
      input_call_r2_edit_->setFont(input_font);
      input_rst_r2_edit_->setFont(input_font);
    }

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

    function_panel_ = new QFrame(central);
    {
      auto *function_layout = new QHBoxLayout(function_panel_);
      function_layout->setContentsMargins(4, 2, 4, 2);
      function_layout->setSpacing(3);
      for (int i = 0; i < CW_KEY_COUNT; i++) {
        fn_buttons_[i] = new QPushButton(QString("F%1").arg(i + 1), function_panel_);
        fn_buttons_[i]->setFocusPolicy(Qt::NoFocus);
        fn_buttons_[i]->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        connect(fn_buttons_[i], &QPushButton::clicked, this, [this, i]() {
          on_cw_fn_key(i + 1);
          refresh_ui();
        });
        function_layout->addWidget(fn_buttons_[i]);
      }
    }
    left_layout->addWidget(function_panel_);

    dxcc_panel_ = new QFrame(central);
    auto *dxcc_layout = new QHBoxLayout(dxcc_panel_);
    dxcc_layout->setContentsMargins(8, 2, 8, 2);
    dxcc_label_ = new QLabel(dxcc_panel_);
    dxcc_label_->setWordWrap(false);
    dxcc_layout->addWidget(dxcc_label_);

    stats_panel_ = new QFrame(central);
    auto *stats_layout = new QHBoxLayout(stats_panel_);
    stats_layout->setContentsMargins(8, 2, 8, 2);
    stats_label_ = new QLabel(stats_panel_);
    stats_label_->setWordWrap(false);
    stats_layout->addWidget(stats_label_);

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
        cat_conn_badge_label_ = new ClickableStatusLabel("CAT R1 OFF", op_panel_);
        cat_conn_badge_r2_label_ = new ClickableStatusLabel("CAT R2 OFF", op_panel_);
        cw_conn_badge_label_ = new ClickableStatusLabel("CW R1 OFF", op_panel_);
        cw_conn_badge_r2_label_ = new ClickableStatusLabel("CW R2 OFF", op_panel_);
        cat_conn_badge_label_->setWordWrap(false);
        cat_conn_badge_r2_label_->setWordWrap(false);
        cw_conn_badge_label_->setWordWrap(false);
        cw_conn_badge_r2_label_->setWordWrap(false);
        cat_conn_badge_label_->setAlignment(Qt::AlignCenter);
        cat_conn_badge_r2_label_->setAlignment(Qt::AlignCenter);
        cw_conn_badge_label_->setAlignment(Qt::AlignCenter);
        cw_conn_badge_r2_label_->setAlignment(Qt::AlignCenter);
        cat_conn_badge_label_->setMinimumWidth(96);
        cat_conn_badge_r2_label_->setMinimumWidth(96);
        cw_conn_badge_label_->setMinimumWidth(96);
        cw_conn_badge_r2_label_->setMinimumWidth(96);
        cat_conn_badge_label_->setMinimumHeight(22);
        cat_conn_badge_r2_label_->setMinimumHeight(22);
        cw_conn_badge_label_->setMinimumHeight(22);
        cw_conn_badge_r2_label_->setMinimumHeight(22);
        op_layout->addWidget(cat_conn_badge_label_, 1, 5, 1, 1, Qt::AlignLeft);
        op_layout->addWidget(cat_conn_badge_r2_label_, 1, 6, 1, 1, Qt::AlignLeft);
        op_layout->addWidget(cw_conn_badge_label_, 2, 5, 1, 1, Qt::AlignLeft);
        op_layout->addWidget(cw_conn_badge_r2_label_, 2, 6, 1, 1, Qt::AlignLeft);
        cat_conn_badge_r2_label_->setVisible(false);
        cw_conn_badge_r2_label_->setVisible(false);
        cat_conn_badge_label_->set_on_click([this]() { on_cat_badge_toggle(CAT_SLOT_A); });
        cat_conn_badge_r2_label_->set_on_click([this]() { on_cat_badge_toggle(CAT_SLOT_B); });
        cw_conn_badge_label_->set_on_click([this]() { on_cw_badge_toggle(); });
        cw_conn_badge_r2_label_->set_on_click([this]() { on_cw_badge_toggle(); });

        radio1_label_ = new QLabel("R1 --", op_panel_);
        radio2_label_ = new QLabel("R2 --", op_panel_);
        op_layout->addWidget(radio1_label_, 1, 0, 1, 2);
        op_layout->addWidget(radio2_label_, 2, 0, 1, 2);

        op_layout->addWidget(new QLabel("TX Exchange:", op_panel_), 2, 2);
        contest_tx_exchange_edit_ = new QLineEdit(op_panel_);
        contest_tx_exchange_edit_->clear();
        contest_tx_exchange_edit_->setPlaceholderText("auto (from EXCHANGE_SENT)");
        contest_tx_exchange_edit_->setToolTip(
          "Read-only preview of current TX exchange generated from contest definition.");
        contest_tx_exchange_edit_->setReadOnly(true);
        op_layout->addWidget(contest_tx_exchange_edit_, 2, 3, 1, 2);

        op_layout->addWidget(new QLabel("CW WPM:", op_panel_), 3, 2);
        cw_wpm_spin_ = new QSpinBox(op_panel_);
        cw_wpm_spin_->setRange(1, 60);
        cw_wpm_spin_->setValue(config.cw_wpm);
        cw_wpm_spin_->setMaximumWidth(72);
        cw_wpm_spin_->setToolTip("Set current CW keyer speed immediately.");
        connect(cw_wpm_spin_, QOverload<int>::of(&QSpinBox::valueChanged), this,
                [this](int value) {
                  config.cw_wpm = value;
                  config_save("logger.conf");
                  cat_set_cw_wpm(value);
                });
        op_layout->addWidget(cw_wpm_spin_, 3, 3, 1, 1);

        export_filename_edit_ = new QLineEdit(op_panel_);
        export_filename_edit_->setPlaceholderText("export filename");
        export_filename_edit_->setToolTip("Filename for ADIF/Cabrillo export");
        export_filename_edit_->setClearButtonEnabled(true);
        export_filename_edit_->hide();
        op_layout->addWidget(export_filename_edit_, 3, 0, 1, 6);
        connect(export_filename_edit_, &QLineEdit::textEdited, this,
                [this](const QString &text) {
                  if (app_controller_export_prompt_active())
                    app_controller_set_export_filename_text(text.toLatin1().constData());
                });

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

    auto *cluster_buttons_row = new QFrame(cluster_group_);
    auto *cluster_buttons_layout = new QHBoxLayout(cluster_buttons_row);
    cluster_buttons_layout->setContentsMargins(0, 0, 0, 0);
    cluster_buttons_layout->setSpacing(6);
    auto *dxcluster_retry_button = new QPushButton("Reconnect", cluster_group_);
    cluster_buttons_layout->addWidget(dxcluster_retry_button, 0);
    cluster_layout->addWidget(cluster_buttons_row);
    connect(dxcluster_retry_button, &QPushButton::clicked, this, []() {
      dxcluster_retry_connection();
    });

        cluster_group_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        left_layout->addWidget(cluster_group_, 1);
    left_layout->addWidget(dxcc_panel_);
    left_layout->addWidget(stats_panel_);

    bandmap2_group_ = new QGroupBox("Bandmap R2", central);
    bandmap2_group_->setObjectName("bandmapGroupR2");
    auto *bandmap2_layout = new QVBoxLayout(bandmap2_group_);
    bandmap2_layout->setContentsMargins(6, 24, 6, 6);
    bandmap2_layout->setSpacing(4);
    bandmap2_label_ = new QLabel("Bandmap R2", bandmap2_group_);
    bandmap2_layout->addWidget(bandmap2_label_);
    bandmap2_list_ = new QListWidget(bandmap2_group_);
    bandmap2_list_->setSelectionMode(QAbstractItemView::SingleSelection);
    bandmap2_list_->setFocusPolicy(Qt::NoFocus);
    bandmap2_list_->setUniformItemSizes(true);
    bandmap2_layout->addWidget(bandmap2_list_, 1);
    bandmap2_group_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    bandmap2_group_->setMinimumWidth(170);

    connect(bandmap2_list_, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *item) {
              if (!item)
                return;
              tune_to_bandmap_row(bandmap2_list_->row(item), 2);
            });

    auto *main_splitter = new QSplitter(Qt::Horizontal, central);
    main_splitter->setObjectName("mainSplitter");
    main_splitter->setChildrenCollapsible(false);
    main_splitter->setHandleWidth(6);
    main_splitter->addWidget(bandmap_group_);
    main_splitter->addWidget(left_column);
    main_splitter->addWidget(bandmap2_group_);
    main_splitter->setStretchFactor(0, 1);
    main_splitter->setStretchFactor(1, 3);
    main_splitter->setStretchFactor(2, 1);
    main_splitter->setSizes({170, 900, 170});
    root->addWidget(main_splitter);

    apply_theme();
    apply_character_cell_layout();

      menuBar()->setNativeMenuBar(false);

      auto trigger_ctrl_key = [this](int app_key) {
        AppControllerEvent ctrl_event = app_controller_handle_key(app_key);
        if (ctrl_event == APP_CTRL_EVENT_REQUEST_CTY_UPDATE)
          start_cty_update_worker();
        if (ctrl_event == APP_CTRL_EVENT_EXIT)
          close();
        if (ctrl_event == APP_CTRL_EVENT_OPEN_QTC_WINDOW)
          qtc_dialog_->showAndFocus();
        refresh_ui();
      };

      QAction *docs_action = new QAction("Documentation", this);
      docs_action->setShortcut(QKeySequence("Ctrl+F1"));
      docs_action->setShortcutContext(Qt::ApplicationShortcut);
      connect(docs_action, &QAction::triggered, this,
              [this]() { documentation_dialog_->showAndFocus(); });

            QAction *new_log_action = new QAction("New Log", this);
      new_log_action->setShortcut(QKeySequence("Ctrl+F2"));
      connect(new_log_action, &QAction::triggered, this, [this]() {
        prompt_create_named_log();
        refresh_ui();
      });

            QAction *open_log_action = new QAction("Open Log", this);
      open_log_action->setShortcut(QKeySequence("Ctrl+F3"));
      connect(open_log_action, &QAction::triggered, this, [this]() {
        prompt_open_named_log();
        refresh_ui();
      });

            QAction *export_action = new QAction("Export", this);
      export_action->setShortcut(QKeySequence("Ctrl+F4"));
      connect(export_action, &QAction::triggered, this,
              [trigger_ctrl_key]() { trigger_ctrl_key(APP_KEY_F4); });

            QAction *station_config_action = new QAction("Station Config", this);
      station_config_action->setShortcut(QKeySequence("Ctrl+Shift+F8"));
      connect(station_config_action, &QAction::triggered, this, [this]() {
        prompt_station_configuration();
        refresh_ui();
      });

            QAction *network_config_action = new QAction("Network Config", this);
      network_config_action->setShortcut(QKeySequence("Ctrl+Shift+F9"));
      connect(network_config_action, &QAction::triggered, this, [this]() {
        prompt_network_configuration();
        refresh_ui();
      });

            QAction *contest_action = new QAction("Contest Config", this);
      contest_action->setShortcut(QKeySequence("Ctrl+F8"));
      connect(contest_action, &QAction::triggered, this, [this]() {
        prompt_contest_configuration();
        refresh_ui();
      });

            QAction *cluster_action = new QAction("DX Cluster", this);
      cluster_action->setShortcut(QKeySequence("Ctrl+F5"));
      connect(cluster_action, &QAction::triggered, this,
              [trigger_ctrl_key]() { trigger_ctrl_key(APP_KEY_F5); });

            QAction *stats_action = new QAction("Stats", this);
      stats_action->setShortcut(QKeySequence("Ctrl+F6"));
      connect(stats_action, &QAction::triggered, this,
              [trigger_ctrl_key]() { trigger_ctrl_key(APP_KEY_F6); });

            QAction *toggle_bandmap_r1_action = new QAction("Show Bandmap R1", this);
          toggle_bandmap_r1_action->setCheckable(true);
          toggle_bandmap_r1_action->setShortcut(QKeySequence("Ctrl+B"));
          toggle_bandmap_r1_action->setShortcutContext(Qt::ApplicationShortcut);
          toggle_bandmap_r1_action->setChecked(bandmap_r1_visible_);
          connect(toggle_bandmap_r1_action, &QAction::toggled, this,
              [this](bool checked) {
                bandmap_r1_visible_ = checked;
                refresh_ui();
              });
          toggle_bandmap_r1_action_ = toggle_bandmap_r1_action;

            QAction *toggle_bandmap_r2_action = new QAction("Show Bandmap R2", this);
          toggle_bandmap_r2_action->setCheckable(true);
          toggle_bandmap_r2_action->setShortcut(QKeySequence("Ctrl+D"));
          toggle_bandmap_r2_action->setShortcutContext(Qt::ApplicationShortcut);
          toggle_bandmap_r2_action->setChecked(bandmap_r2_visible_);
          connect(toggle_bandmap_r2_action, &QAction::toggled, this,
              [this](bool checked) {
                bandmap_r2_visible_ = checked;
                refresh_ui();
              });
          toggle_bandmap_r2_action_ = toggle_bandmap_r2_action;

            QAction *cty_action = new QAction("Update CTY", this);
      cty_action->setShortcut(QKeySequence("Ctrl+F7"));
      connect(cty_action, &QAction::triggered, this,
              [trigger_ctrl_key]() { trigger_ctrl_key(APP_KEY_F7); });

      QAction *scp_action = new QAction("Update SCP (Check Partial)", this);
      connect(scp_action, &QAction::triggered, this, [this]() {
        start_scp_update_worker();
      });

            QAction *cw_action = new QAction("CW Keyer", this);
      cw_action->setShortcut(QKeySequence("Ctrl+K"));
      connect(cw_action, &QAction::triggered, this, [this]() {
        cw_dialog_->showAndFocus();
      });

            QAction *cw_messages_action = new QAction("CW Messages (F1-F10)", this);
      cw_messages_action->setShortcut(QKeySequence("Ctrl+Shift+K"));
      cw_messages_action->setShortcutContext(Qt::ApplicationShortcut);
      connect(cw_messages_action, &QAction::triggered, this, [this]() {
        open_cw_messages_config_dialog();
      });

            QAction *qtc_action = new QAction("QTC Exchange", this);
      qtc_action->setShortcut(QKeySequence("Ctrl+L"));
      qtc_action->setShortcutContext(Qt::ApplicationShortcut);
      connect(qtc_action, &QAction::triggered, this, [this]() {
        if (!app_controller_qtc_enabled()) {
          AppRenderState state;
          app_controller_get_render_state(&state);
          (void)state;
          /* Re-use status via a key trigger so the status bar updates. */
          AppControllerEvent ev = app_controller_handle_key(APP_KEY_CTRL_L);
          if (ev == APP_CTRL_EVENT_NONE)
            refresh_ui();
        } else {
          qtc_dialog_->showAndFocus();
          refresh_ui();
        }
      });

            QAction *qtc_history_action = new QAction("QTC History", this);
      qtc_history_action->setShortcut(QKeySequence("Ctrl+Shift+L"));
      qtc_history_action->setShortcutContext(Qt::ApplicationShortcut);
      connect(qtc_history_action, &QAction::triggered, this, [this]() {
        qtc_history_dialog_->showAndFocus();
        refresh_ui();
      });

            QAction *quit_action = new QAction("Quit", this);
      quit_action->setShortcut(QKeySequence("Ctrl+F10"));
      connect(quit_action, &QAction::triggered, this,
              [trigger_ctrl_key]() { trigger_ctrl_key(APP_KEY_F10); });

      QMenu *main_menu = menuBar()->addMenu("Menu");
      main_menu->addAction(docs_action);
      main_menu->addSeparator();
      main_menu->addAction(new_log_action);
      main_menu->addAction(open_log_action);
      main_menu->addAction(export_action);
      main_menu->addSeparator();
      main_menu->addAction(station_config_action);
      main_menu->addAction(network_config_action);
      main_menu->addAction(contest_action);
      main_menu->addAction(cty_action);
      main_menu->addAction(scp_action);
      main_menu->addSeparator();
      main_menu->addAction(cluster_action);
      main_menu->addAction(stats_action);
      main_menu->addSeparator();
      main_menu->addAction(toggle_bandmap_r1_action);
      main_menu->addAction(toggle_bandmap_r2_action);
      main_menu->addSeparator();
      main_menu->addAction(cw_action);
      main_menu->addAction(cw_messages_action);
      main_menu->addAction(qtc_action);
      main_menu->addAction(qtc_history_action);
      main_menu->addSeparator();
      main_menu->addAction(quit_action);

    cw_dialog_ = new CwKeyerDialog(this);
    cw_messages_dialog_ = new CwMessagesConfigDialog(this);
    cw_messages_dialog_->set_save_handler([this](const CwKeyMap &map) {
      return save_cw_keys_file(map);
    });
    cw_messages_dialog_->set_load_handler([this]() {
      return cw_key_map_;
    });
    cw_messages_dialog_->set_reload_handler([this]() {
      reload_cw_maps_from_disk();
    });
    documentation_dialog_ = new DocumentationDialog(this);

    /* Pre-load contest presets once so dialogs open instantly without
     * blocking the main thread on filesystem scans. */
    contest_presets_cache_ = discover_contest_presets();

    /* try app dir (build), then CWD, then one level up (source tree) */
    reload_cw_maps_from_disk();
    scp_load("MASTER.SCP");

    qtc_dialog_ = new QtcDialog(&cw_qtc_map_, this);
    qtc_history_dialog_ = new QtcHistoryDialog(this);

    timer_ = new QTimer(this);
    connect(timer_, &QTimer::timeout, this, [this]() { refresh_ui(); });
    timer_->start(120);

    start_auto_connect_if_configured();

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

    if (ctrl && event->key() == Qt::Key_Down) {
      navigate_bandmap(+1);
      event->accept();
      return;
    }

    if (ctrl && event->key() == Qt::Key_Up) {
      navigate_bandmap(-1);
      event->accept();
      return;
    }

    if (ctrl && event->key() == Qt::Key_F2) {
      prompt_create_named_log();
      refresh_ui();
      event->accept();
      return;
    }

    if (ctrl && event->key() == Qt::Key_F1) {
      documentation_dialog_->showAndFocus();
      event->accept();
      return;
    }

    if (ctrl && event->key() == Qt::Key_F3) {
      prompt_open_named_log();
      refresh_ui();
      event->accept();
      return;
    }

      if (ctrl && event->key() == Qt::Key_F8) {
        prompt_contest_configuration();
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
      start_cty_update_worker();
      refresh_ui();
    }

    if (ctrl_event == APP_CTRL_EVENT_EXIT)
      close();

    if (ctrl_event == APP_CTRL_EVENT_OPEN_QTC_WINDOW) {
      qtc_dialog_->showAndFocus();
    }

    refresh_ui();
    event->accept();
  }

private:
  void start_cty_update_worker() {
    if (cty_update_worker_running_)
      return;

    cty_update_worker_running_ = true;
    std::thread([this]() {
      const int download_ok = cty_download_latest("wl_cty.dat") == 0 ? 1 : 0;
      QMetaObject::invokeMethod(
          this,
          [this, download_ok]() {
            app_controller_complete_cty_update(download_ok);
            cty_update_worker_running_ = false;
            refresh_ui();
          },
          Qt::QueuedConnection);
    }).detach();
  }

  void submit_command_text(const QString &command_text) {
    const QByteArray bytes = command_text.toUtf8();
    AppControllerEvent ctrl_event =
        app_controller_submit_command_text(bytes.constData());
    if (ctrl_event == APP_CTRL_EVENT_EXIT)
      close();
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

    /* Use cached preset list (populated once at construction). */
    const auto &presets = contest_presets_cache_;

    QStringList contest_options;
    contest_options << "None";
    for (const auto &p : presets)
      contest_options << p.first;

    /* Fallback to compiled-in list when directory cannot be found. */
    if (presets.empty()) {
      for (const auto &preset : kContestPresets)
        contest_options << QString::fromLatin1(preset.label);
    }

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

    /* Try dynamic list first. */
    for (const auto &p : presets) {
      if (selected_contest == p.first) {
        submit_command_text(QString("contest %1").arg(p.second));
        return;
      }
    }

    /* Fallback to compiled-in list. */
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

    void prompt_operator_call() {
      bool ok = false;
      const QString current = QString::fromLatin1(config_effective_operator_call());
      const QString text = QInputDialog::getText(this, "Operator callsign",
                                                "OPERATOR_CALL:",
                                                QLineEdit::Normal,
                                                current, &ok);
      if (!ok)
        return;

      QByteArray bytes = text.trimmed().toLatin1();
      if (bytes.isEmpty()) {
        snprintf(config.operator_call, sizeof(config.operator_call), "%s",
                 config.station_call);
      } else {
        snprintf(config.operator_call, sizeof(config.operator_call), "%s",
                 bytes.constData());
      }

      if (config_save_active() != 0) {
        QMessageBox::warning(this, "Operator callsign",
                             "Nie mozna zapisac logger.conf.");
        return;
      }

      refresh_ui();
    }

    void prompt_station_configuration() {
      QDialog dialog(this);
      dialog.setWindowTitle("Konfiguracja stacji");
      dialog.setModal(true);
      dialog.resize(760, 560);

      auto *layout = new QVBoxLayout(&dialog);
      auto *hint = new QLabel(
          "Edycja pól zapisanych do logger.conf. Zmiany zapisywane sa na stałe po kliknieciu OK.",
          &dialog);
      hint->setWordWrap(true);
      layout->addWidget(hint);

      auto *general_group = new QGroupBox("General", &dialog);
      auto *general_form = new QFormLayout(general_group);
      auto *station_call_edit = new QLineEdit(QString::fromLatin1(config.station_call), &dialog);
      general_form->addRow("STATION_CALL:", station_call_edit);
      auto *operator_call_edit = new QLineEdit(QString::fromLatin1(config.operator_call), &dialog);
      general_form->addRow("OPERATOR_CALL:", operator_call_edit);
      auto *operator_name_edit = new QLineEdit(QString::fromLatin1(config.operator_name), &dialog);
      general_form->addRow("OPERATOR_NAME:", operator_name_edit);
      auto *locator_edit = new QLineEdit(QString::fromLatin1(config.locator), &dialog);
      general_form->addRow("LOCATOR:", locator_edit);
      auto *ui_theme_monokai_check = new QCheckBox(&dialog);
      ui_theme_monokai_check->setChecked(config.ui_monokai_theme != 0);
      general_form->addRow("UI_THEME_MONOKAI:", ui_theme_monokai_check);
      layout->addWidget(general_group);

      auto *dx_group = new QGroupBox("DX Cluster", &dialog);
      auto *dx_form = new QFormLayout(dx_group);
      auto *dxc_host_edit = new QLineEdit(QString::fromLatin1(config.dxc_host), &dialog);
      dx_form->addRow("DXC_HOST:", dxc_host_edit);
      auto *dxc_port_spin = new QSpinBox(&dialog);
      dxc_port_spin->setRange(1, 65535);
      dxc_port_spin->setValue(config.dxc_port);
      dx_form->addRow("DXC_PORT:", dxc_port_spin);
      auto *dxc_call_edit = new QLineEdit(QString::fromLatin1(config.dxc_call), &dialog);
      dx_form->addRow("DXC_CALL:", dxc_call_edit);
      layout->addWidget(dx_group);

      auto *cat_group = new QGroupBox("CAT", &dialog);
      auto *cat_form = new QFormLayout(cat_group);
      CatRigInfo cat_rigs[256];
      const int cat_rig_count = cat_list_rigs(cat_rigs, 256);
      auto *cat_model_combo = new QComboBox(&dialog);
      if (cat_rig_count > 0) {
        int selected_index = 0;
        for (int i = 0; i < cat_rig_count; i++) {
          cat_model_combo->addItem(QString::fromLatin1(cat_rigs[i].model_name), cat_rigs[i].model);
          if (cat_rigs[i].model == config.cat_model)
            selected_index = i;
        }
        cat_model_combo->setCurrentIndex(selected_index);
      } else {
        cat_model_combo->addItem("No rigs available", 0);
        cat_model_combo->setCurrentIndex(0);
      }
      cat_form->addRow("CAT_MODEL:", cat_model_combo);
      auto *cat_device_edit = new QLineEdit(QString::fromLatin1(config.cat_device), &dialog);
      cat_form->addRow("CAT_DEVICE:", cat_device_edit);
      auto *cat_baud_spin = new QSpinBox(&dialog);
      cat_baud_spin->setRange(1200, 115200);
      cat_baud_spin->setValue(config.cat_baud);
      cat_form->addRow("CAT_BAUD:", cat_baud_spin);
      auto *cat_data_bits_spin = new QSpinBox(&dialog);
      cat_data_bits_spin->setRange(5, 8);
      cat_data_bits_spin->setValue(config.cat_data_bits);
      cat_form->addRow("CAT_DATA_BITS:", cat_data_bits_spin);
      auto *cat_stop_bits_spin = new QSpinBox(&dialog);
      cat_stop_bits_spin->setRange(1, 2);
      cat_stop_bits_spin->setValue(config.cat_stop_bits);
      cat_form->addRow("CAT_STOP_BITS:", cat_stop_bits_spin);
      auto *cat_parity_combo = new QComboBox(&dialog);
      cat_parity_combo->addItems({"None", "Even", "Odd"});
      cat_parity_combo->setCurrentText(QString::fromLatin1(config.cat_parity));
      cat_form->addRow("CAT_PARITY:", cat_parity_combo);
      auto *cat_handshake_combo = new QComboBox(&dialog);
      cat_handshake_combo->addItems({"None", "XON/XOFF", "RTS/CTS"});
      cat_handshake_combo->setCurrentText(QString::fromLatin1(config.cat_handshake));
      cat_form->addRow("CAT_HANDSHAKE:", cat_handshake_combo);
      auto *cat_mode_from_rig = new QCheckBox(&dialog);
      cat_mode_from_rig->setChecked(config.cat_mode_from_rig != 0);
      cat_form->addRow("CAT_MODE_FROM_RIG:", cat_mode_from_rig);
      auto *cat2_model_combo = new QComboBox(&dialog);
      int cat2_selected_index = 0;
      if (cat_rig_count > 0) {
        for (int i = 0; i < cat_rig_count; i++) {
          cat2_model_combo->addItem(QString::fromLatin1(cat_rigs[i].model_name), cat_rigs[i].model);
          if (cat_rigs[i].model == config.cat2_model)
            cat2_selected_index = i;
        }
        cat2_model_combo->setCurrentIndex(cat2_selected_index);
      } else {
        cat2_model_combo->addItem("No rigs available", 0);
        cat2_model_combo->setCurrentIndex(0);
      }
      cat_form->addRow("CAT2_MODEL:", cat2_model_combo);
      auto *cat2_device_edit = new QLineEdit(QString::fromLatin1(config.cat2_device), &dialog);
      cat_form->addRow("CAT2_DEVICE:", cat2_device_edit);
      auto *cat2_baud_spin = new QSpinBox(&dialog);
      cat2_baud_spin->setRange(1200, 115200);
      cat2_baud_spin->setValue(config.cat2_baud);
      cat_form->addRow("CAT2_BAUD:", cat2_baud_spin);
      auto *cat2_data_bits_spin = new QSpinBox(&dialog);
      cat2_data_bits_spin->setRange(5, 8);
      cat2_data_bits_spin->setValue(config.cat2_data_bits);
      cat_form->addRow("CAT2_DATA_BITS:", cat2_data_bits_spin);
      auto *cat2_stop_bits_spin = new QSpinBox(&dialog);
      cat2_stop_bits_spin->setRange(1, 2);
      cat2_stop_bits_spin->setValue(config.cat2_stop_bits);
      cat_form->addRow("CAT2_STOP_BITS:", cat2_stop_bits_spin);
      auto *cat2_parity_combo = new QComboBox(&dialog);
      cat2_parity_combo->addItems({"None", "Even", "Odd"});
      cat2_parity_combo->setCurrentText(QString::fromLatin1(config.cat2_parity));
      cat_form->addRow("CAT2_PARITY:", cat2_parity_combo);
      auto *cat2_handshake_combo = new QComboBox(&dialog);
      cat2_handshake_combo->addItems({"None", "XON/XOFF", "RTS/CTS"});
      cat2_handshake_combo->setCurrentText(QString::fromLatin1(config.cat2_handshake));
      cat_form->addRow("CAT2_HANDSHAKE:", cat2_handshake_combo);
      layout->addWidget(cat_group);

      auto *cw_group = new QGroupBox("CW", &dialog);
      auto *cw_form = new QFormLayout(cw_group);
      auto *cw_device_edit = new QLineEdit(QString::fromLatin1(config.cw_device), &dialog);
      cw_form->addRow("CW_DEVICE:", cw_device_edit);
      auto *cw_keyer_line_combo = new QComboBox(&dialog);
      cw_keyer_line_combo->addItems({"DTR", "RTS"});
      cw_keyer_line_combo->setCurrentText(QString::fromLatin1(config.cw_keyer_line));
      cw_form->addRow("CW_KEYER_LINE:", cw_keyer_line_combo);
      auto *cw_wpm_spin = new QSpinBox(&dialog);
      cw_wpm_spin->setRange(5, 60);
      cw_wpm_spin->setValue(config.cw_wpm);
      cw_form->addRow("CW_WPM:", cw_wpm_spin);
      layout->addWidget(cw_group);

      auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                           Qt::Horizontal, &dialog);
      connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
      connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
      layout->addWidget(buttons);

      if (dialog.exec() != QDialog::Accepted)
        return;

      auto copy_field = [](char *dst, size_t dst_size, const QString &text) {
        QByteArray bytes = text.trimmed().toLatin1();
        std::snprintf(dst, dst_size, "%s", bytes.constData());
      };

      copy_field(config.station_call, sizeof(config.station_call), station_call_edit->text());
      copy_field(config.operator_call, sizeof(config.operator_call), operator_call_edit->text());
      if (!config.operator_call[0])
        snprintf(config.operator_call, sizeof(config.operator_call), "%s", config.station_call);
      copy_field(config.operator_name, sizeof(config.operator_name), operator_name_edit->text());
      copy_field(config.locator, sizeof(config.locator), locator_edit->text());
      config.ui_monokai_theme = ui_theme_monokai_check->isChecked() ? 1 : 0;
      copy_field(config.dxc_host, sizeof(config.dxc_host), dxc_host_edit->text());
      config.dxc_port = dxc_port_spin->value();
      copy_field(config.dxc_call, sizeof(config.dxc_call), dxc_call_edit->text());
      config.cat_model = cat_model_combo->currentData().toInt();
      copy_field(config.cat_device, sizeof(config.cat_device), cat_device_edit->text());
      config.cat_baud = cat_baud_spin->value();
      config.cat_data_bits = cat_data_bits_spin->value();
      config.cat_stop_bits = cat_stop_bits_spin->value();
      copy_field(config.cat_parity, sizeof(config.cat_parity), cat_parity_combo->currentText());
      copy_field(config.cat_handshake, sizeof(config.cat_handshake), cat_handshake_combo->currentText());
      config.cat_mode_from_rig = cat_mode_from_rig->isChecked() ? 1 : 0;
      config.cat2_model = cat2_model_combo->currentData().toInt();
      copy_field(config.cat2_device, sizeof(config.cat2_device), cat2_device_edit->text());
      config.cat2_baud = cat2_baud_spin->value();
      config.cat2_data_bits = cat2_data_bits_spin->value();
      config.cat2_stop_bits = cat2_stop_bits_spin->value();
      copy_field(config.cat2_parity, sizeof(config.cat2_parity), cat2_parity_combo->currentText());
      copy_field(config.cat2_handshake, sizeof(config.cat2_handshake), cat2_handshake_combo->currentText());
      copy_field(config.cw_device, sizeof(config.cw_device), cw_device_edit->text());
      copy_field(config.cw_keyer_line, sizeof(config.cw_keyer_line), cw_keyer_line_combo->currentText());
      config.cw_wpm = cw_wpm_spin->value();

      if (config_save("logger.conf") != 0) {
        QMessageBox::warning(this, "Konfiguracja stacji",
                             "Nie mozna zapisac logger.conf.");
        return;
      }

      apply_theme();
      refresh_ui();
    }

    void prompt_network_configuration() {
      QDialog dialog(this);
      dialog.setWindowTitle("Konfiguracja sieci");
      dialog.setModal(true);
      dialog.resize(620, 460);

      auto *layout = new QVBoxLayout(&dialog);
      auto *hint = new QLabel(
          "Konfiguracja pracy sieciowej. Zmiany sa zapisywane do logger.conf.",
          &dialog);
      hint->setWordWrap(true);
      layout->addWidget(hint);

      auto *form = new QFormLayout();
      form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

      auto *net_enabled_check = new QCheckBox(&dialog);
      net_enabled_check->setChecked(config.net_enabled != 0);
      form->addRow("NET_ENABLED:", net_enabled_check);

      auto *station_name_edit =
          new QLineEdit(QString::fromLatin1(config.net_station_id), &dialog);
      station_name_edit->setPlaceholderText("np. RUN1");
      form->addRow("NET_STATION_ID:", station_name_edit);

      auto *role_combo = new QComboBox(&dialog);
      role_combo->addItem("client", "client");
      role_combo->addItem("server", "server");
      const int role_index =
          QString::fromLatin1(config.net_role).trimmed().compare("server", Qt::CaseInsensitive) == 0
              ? 1
              : 0;
      role_combo->setCurrentIndex(role_index);
      form->addRow("NET_ROLE:", role_combo);

      auto *host_edit =
          new QLineEdit(QString::fromLatin1(config.net_server_host), &dialog);
      host_edit->setPlaceholderText("127.0.0.1 lub nazwa hosta");
      host_edit->setVisible(role_combo->currentData().toString().compare("server", Qt::CaseInsensitive) != 0);
      host_edit->setEnabled(host_edit->isVisible());
      form->addRow("NET_SERVER_HOST (client only):", host_edit);
      QObject::connect(role_combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                       [&](int) {
                         const bool is_server =
                             role_combo->currentData().toString().compare("server", Qt::CaseInsensitive) == 0;
                         host_edit->setVisible(!is_server);
                         host_edit->setEnabled(!is_server);
                       });

      auto *port_spin = new QSpinBox(&dialog);
      port_spin->setRange(1, 65535);
      port_spin->setValue(config.net_server_port);
      form->addRow("NET_SERVER_PORT:", port_spin);

        auto *tls_check = new QCheckBox(&dialog);
        tls_check->setChecked(config.net_tls != 0);
        form->addRow("NET_TLS:", tls_check);

        auto *shared_key_edit =
          new QLineEdit(QString::fromLatin1(config.net_shared_key), &dialog);
        shared_key_edit->setEchoMode(QLineEdit::PasswordEchoOnEdit);
        shared_key_edit->setPlaceholderText("wspolny klucz autoryzacji");
        form->addRow("NET_SHARED_KEY:", shared_key_edit);

        auto *fingerprint_edit = new QLineEdit(
          QString::fromLatin1(config.net_tls_peer_fingerprint), &dialog);
        fingerprint_edit->setPlaceholderText(
          "SHA-256 fingerprint serwera, np. AA:BB:CC:...");
        form->addRow("NET_TLS_PEER_FINGERPRINT:", fingerprint_edit);

      layout->addLayout(form);

      auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                           Qt::Horizontal, &dialog);
      connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
      connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
      layout->addWidget(buttons);

      if (dialog.exec() != QDialog::Accepted)
        return;

      auto copy_field = [](char *dst, size_t dst_size, const QString &text) {
        QByteArray bytes = text.trimmed().toLatin1();
        std::snprintf(dst, dst_size, "%s", bytes.constData());
      };

      config.net_enabled = net_enabled_check->isChecked() ? 1 : 0;
      copy_field(config.net_station_id, sizeof(config.net_station_id),
                 station_name_edit->text());
      copy_field(config.net_role, sizeof(config.net_role),
                 role_combo->currentData().toString());
      copy_field(config.net_server_host, sizeof(config.net_server_host),
                 host_edit->text());
      config.net_server_port = port_spin->value();
      config.net_tls = tls_check->isChecked() ? 1 : 0;
      copy_field(config.net_shared_key, sizeof(config.net_shared_key),
                 shared_key_edit->text());
      copy_field(config.net_auth_token, sizeof(config.net_auth_token),
                 shared_key_edit->text());
      copy_field(config.net_tls_peer_fingerprint,
                 sizeof(config.net_tls_peer_fingerprint),
                 fingerprint_edit->text());

      if (config_save_active() != 0) {
        QMessageBox::warning(this, "Konfiguracja sieci",
                             "Nie mozna zapisac logger.conf.");
        return;
      }

      const bool was_enabled = net_enabled_check->isChecked();
      submit_command_text("net off");
      if (was_enabled)
        submit_command_text("net on");

      QMessageBox::information(this, "Konfiguracja sieci",
                               was_enabled
                                   ? "Ustawienia sieci zapisane i sync zostal automatycznie zrestartowany."
                                   : "Ustawienia sieci zapisane. Sync pozostaje wylaczony.");
      refresh_ui();
    }

    void prompt_contest_configuration() {
      ContestDefinition def;
      contest_definition_init_defaults(&def);

      QString def_path = QString::fromLatin1(config.contest_definition_path).trimmed();
      if (def_path.isEmpty())
        def_path = "contest.conf";

      {
        QByteArray path_bytes = def_path.toLocal8Bit();
        char load_err[128] = {0};
        if (contest_definition_load(path_bytes.constData(), &def, load_err,
                                    sizeof(load_err)) != 0) {
          contest_definition_init_defaults(&def);
        }
      }

      QDialog dialog(this);
      dialog.setWindowTitle("Konfiguracja zawodow");
      dialog.setModal(true);
      dialog.resize(640, 430);

      auto *layout = new QVBoxLayout(&dialog);
      auto *hint = new QLabel(
          "Podglad i edycja opcji zawodow. Zmiany zapisywane sa do pliku definicji i logger.conf.",
          &dialog);
      hint->setWordWrap(true);
      layout->addWidget(hint);

      auto *form = new QFormLayout();
      form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

      /* --- Contest preset selector (dynamic from contest_defs/) --- */
      auto *preset_combo = new QComboBox(&dialog);
      preset_combo->addItem("-- wpisz plik recznie / keep current --", "");
      const auto &presets = contest_presets_cache_;
      for (const auto &p : presets)
        preset_combo->addItem(p.first, p.second);
      /* Fallback: compiled-in list when directory not found. */
      if (presets.empty()) {
        for (const auto &preset : kContestPresets)
          preset_combo->addItem(QString::fromLatin1(preset.label),
                                QString::fromLatin1(preset.path));
      }
      form->addRow("Preset zawodow:", preset_combo);

      auto *path_edit = new QLineEdit(def_path, &dialog);
      form->addRow("Plik definicji:", path_edit);

      auto *technique_combo = new QComboBox(&dialog);
      technique_combo->addItems({"SO1R", "SO2V", "SO2R"});
      technique_combo->setCurrentIndex(
          technique_combo_index(config.contest_technique));
      form->addRow("Technika:", technique_combo);

      auto *tx_exchange_edit =
          new QLineEdit(QString::fromLatin1(config.contest_tx_exchange), &dialog);
      tx_exchange_edit->setPlaceholderText("puste = auto z EXCHANGE_SENT");
      form->addRow("TX exchange override:", tx_exchange_edit);

      auto *name_edit = new QLineEdit(QString::fromLatin1(def.name), &dialog);
      form->addRow("Nazwa:", name_edit);

      auto *cabrillo_name_edit =
          new QLineEdit(QString::fromLatin1(def.cabrillo_name), &dialog);
      form->addRow("Cabrillo name:", cabrillo_name_edit);

      auto *mode_combo = new QComboBox(&dialog);
      mode_combo->addItems({"MIXED", "CW", "SSB", "PHONE", "RTTY", "DIGI"});
      mode_combo->setEditable(true);
      mode_combo->setCurrentText(QString::fromLatin1(def.mode));
      form->addRow("Mode:", mode_combo);

      /* Wire preset selector: selecting a preset fills path and reloads
       * NAME/CABRILLO_NAME/MODE from the file. */
      connect(preset_combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
              &dialog, [&](int /*idx*/) {
                const QString selected_path =
                    preset_combo->currentData().toString();
                if (selected_path.isEmpty())
                  return;

                path_edit->setText(selected_path);

                ContestDefinition loaded;
                contest_definition_init_defaults(&loaded);
                char load_err2[64] = {0};
                const QByteArray pb = selected_path.toLocal8Bit();
                if (contest_definition_load(pb.constData(), &loaded,
                                            load_err2, sizeof(load_err2)) == 0) {
                  name_edit->setText(QString::fromLatin1(loaded.name));
                  cabrillo_name_edit->setText(
                      QString::fromLatin1(loaded.cabrillo_name));
                  mode_combo->setCurrentText(QString::fromLatin1(loaded.mode));
                }
              });

      auto *cat_op_edit =
          new QLineEdit(QString::fromLatin1(def.category_operator), &dialog);
      form->addRow("Category operator:", cat_op_edit);

      auto *cat_band_edit =
          new QLineEdit(QString::fromLatin1(def.category_band), &dialog);
      form->addRow("Category band:", cat_band_edit);

      auto *cat_power_edit =
          new QLineEdit(QString::fromLatin1(def.category_power), &dialog);
      form->addRow("Category power:", cat_power_edit);

      auto *cat_overlay_edit =
          new QLineEdit(QString::fromLatin1(def.category_overlay), &dialog);
      form->addRow("Category overlay:", cat_overlay_edit);

      auto *station_location_edit =
          new QLineEdit(QString::fromLatin1(def.station_location), &dialog);
      form->addRow("Station location:", station_location_edit);

      auto *operators_edit =
          new QLineEdit(QString::fromLatin1(def.operators), &dialog);
      form->addRow("Operators:", operators_edit);

      auto *exchange_template_edit =
          new QLineEdit(QString::fromLatin1(def.exchange_sent_template), &dialog);
      form->addRow("EXCHANGE_SENT:", exchange_template_edit);

      layout->addLayout(form);

      auto *buttons =
          new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                               Qt::Horizontal, &dialog);
      connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
      connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
      layout->addWidget(buttons);

      if (dialog.exec() != QDialog::Accepted)
        return;

      const QString path = path_edit->text().trimmed();
      if (path.isEmpty()) {
        QMessageBox::warning(this, "Konfiguracja zawodow",
                             "Plik definicji nie moze byc pusty.");
        return;
      }

      auto copy_field = [](char *dst, size_t dst_size, const QString &text,
                           bool uppercase) {
        QByteArray bytes = text.trimmed().toLatin1();
        if (uppercase)
          bytes = bytes.toUpper();
        std::snprintf(dst, dst_size, "%s", bytes.constData());
      };

      copy_field(def.name, sizeof(def.name), name_edit->text(), false);
      copy_field(def.cabrillo_name, sizeof(def.cabrillo_name),
                 cabrillo_name_edit->text(), false);
      copy_field(def.mode, sizeof(def.mode), mode_combo->currentText(), true);
      copy_field(def.category_operator, sizeof(def.category_operator),
                 cat_op_edit->text(), true);
      copy_field(def.category_band, sizeof(def.category_band),
                 cat_band_edit->text(), true);
      copy_field(def.category_power, sizeof(def.category_power),
                 cat_power_edit->text(), true);
      copy_field(def.category_overlay, sizeof(def.category_overlay),
                 cat_overlay_edit->text(), true);
      copy_field(def.station_location, sizeof(def.station_location),
                 station_location_edit->text(), true);
      copy_field(def.operators, sizeof(def.operators), operators_edit->text(), true);
      copy_field(def.exchange_sent_template, sizeof(def.exchange_sent_template),
                 exchange_template_edit->text(), false);

      if (!def.name[0])
        std::snprintf(def.name, sizeof(def.name), "%s", "GENERAL");
      if (!def.cabrillo_name[0])
        std::snprintf(def.cabrillo_name, sizeof(def.cabrillo_name), "%s", def.name);
      if (!def.mode[0])
        std::snprintf(def.mode, sizeof(def.mode), "%s", "MIXED");
      if (!def.exchange_sent_template[0])
        std::snprintf(def.exchange_sent_template,
                      sizeof(def.exchange_sent_template), "%s", "#");

      QByteArray path_bytes = path.toLocal8Bit();
      char save_err[128] = {0};
      if (save_contest_definition_file(path_bytes.constData(), &def, save_err,
                                       sizeof(save_err)) != 0) {
        QMessageBox::warning(this, "Konfiguracja zawodow",
                             QString("Nie mozna zapisac definicji: %1")
                                 .arg(QString::fromLatin1(save_err)));
        return;
      }

      std::snprintf(config.contest_definition_path,
                    sizeof(config.contest_definition_path), "%s",
                    path_bytes.constData());

      QByteArray tx_bytes = tx_exchange_edit->text().trimmed().toLatin1();
      std::snprintf(config.contest_tx_exchange,
                    sizeof(config.contest_tx_exchange), "%s",
                    tx_bytes.constData());

      config.contest_technique =
          technique_from_combo_index(technique_combo->currentIndex());
      app_controller_set_contest_technique(config.contest_technique);

      config_save("logger.conf");

      // Queue contest reload after dialog closes to avoid temporary UI freeze
      // on the modal window while stats are recalculated.
      const QString contest_cmd = QString("contest %1").arg(path);
      QTimer::singleShot(0, this, [this, contest_cmd]() {
        submit_command_text(contest_cmd);
        refresh_ui();
      });
    }

  static int translate_key(QKeyEvent *event) {
    const bool ctrl = (event->modifiers() & Qt::ControlModifier) != 0;
    const bool alt = (event->modifiers() & Qt::AltModifier) != 0;
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
    case Qt::Key_W:
      if (alt)
        return APP_KEY_ALT_W;
      break;
    case Qt::Key_L:
      if (ctrl)
        return APP_KEY_CTRL_L;
      break;
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
    const bool monokai = config.ui_monokai_theme != 0;
    const QString bg = monokai ? QString("#272822") : QString("#89d4e8");
    const QString panel = monokai ? QString("#3e3d32") : QString("#9be2ef");
    const QString panel_alt = monokai ? QString("#383830") : QString("#95dceb");
    const QString border = monokai ? QString("#75715e") : QString("#0e6a85");
    const QString text = monokai ? QString("#f8f8f2") : QString("#0d2d3b");
    const QString title_bg = monokai ? QString("#504945") : QString("#0f5ea4");
    const QString title_text = monokai ? QString("#f8f8f2") : QString("#f4f8ff");
    const QString accent = monokai ? QString("#a6e22e") : QString("#f3d24f");
    const QString accent_text = monokai ? QString("#1b1d1e") : QString("#111111");
    const QString status_bg = monokai ? QString("#504945") : QString("#0f5ea4");
    const QString status_text = monokai ? QString("#f8f8f2") : QString("#f4f8ff");
    const QString header_bg = monokai ? QString("#504945") : QString("#0f5ea4");
    const QString header_text = monokai ? QString("#f8f8f2") : QString("#ffe66d");
    const QString button_bg = monokai ? QString("#3e3d32") : QString("#0e4a7a");
    const QString button_text = monokai ? QString("#f8f8f2") : QString("#ffe66d");
    const QString button_hover = monokai ? QString("#6a695f") : QString("#1565a0");

    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSize(10);
    setFont(mono);

    setStyleSheet(
      QString("QMainWindow, QWidget { background: %1; color: %2; }")
          .arg(bg, text)
      + QString("QGroupBox { border: 1px solid %1; border-radius: 3px; margin-top: 8px; background: %2; font-weight: bold; }")
          .arg(border, panel_alt)
      + QString("QGroupBox#logGroup::title { subcontrol-origin: margin; left: 10px; padding: 0 6px; color: %1; background: %2; }")
          .arg(title_text, title_bg)
      + QString("QGroupBox#clusterGroup::title { subcontrol-origin: margin; left: 10px; padding: 0 6px; color: %1; background: %2; }")
          .arg(title_text, title_bg)
      + QString("QHeaderView::section { background: %1; color: %2; padding: 1px 4px; border: 0; font-weight: bold; }")
          .arg(header_bg, header_text)
      + QString("QTableWidget { background: %1; gridline-color: %2; }")
          .arg(panel, border)
      + QString("QLineEdit { background: %1; border: 1px solid %2; color: %3; }")
          .arg(panel, border, text)
      + QString("QFrame#suggestTitle { color: %1; }")
          .arg(title_text)
      + QString("QListWidget { background: %1; border: 1px solid %2; }")
          .arg(panel, border)
      + QString("QListWidget::item:selected { background: %1; color: %2; }")
          .arg(accent, accent_text));

    input_panel_->setStyleSheet(QString("QFrame { background: %1; border: 1px solid %2; }").arg(panel, border));
    status_panel_->setStyleSheet(QString("QFrame { background: %1; color: %2; }").arg(status_bg, status_text));
    dxcc_panel_->setStyleSheet(QString("QFrame { background: %1; color: %2; font-weight: bold; }").arg(status_bg, monokai ? "#fd971f" : "#ffe66d"));
    stats_panel_->setStyleSheet(QString("QFrame { background: %1; color: %2; font-weight: bold; }").arg(status_bg, monokai ? "#fd971f" : "#ffe66d"));
    op_panel_->setStyleSheet(
      QString("QFrame { background: %1; border: 1px solid %2; }")
      .arg(panel_alt, border)
      + QString("QLabel { color: %1; font-weight: bold; }").arg(text));
    gap_panel_->setStyleSheet(QString("QFrame { background: %1; border: 0; }").arg(panel_alt));
    function_panel_->setStyleSheet(
      QString("QFrame { background: %1; }").arg(status_bg)
      + QString(" QPushButton { background: %1; color: %2; font-weight: bold; border: 1px solid %3; border-radius: 3px; padding: 1px 3px; }")
          .arg(button_bg, button_text, border)
      + QString(" QPushButton:hover { background: %1; }").arg(button_hover)
      + QString(" QPushButton:pressed { background: %1; color: %2; }").arg(accent, accent_text));
    suggestions_frame_->setStyleSheet(
        QString("QFrame { background: %1; border: 1px solid %2; }")
            .arg(panel_alt, border)
        + QString("QLabel#suggestTitle { background: %1; color: %2; padding: 2px 6px; font-weight: bold; }")
            .arg(title_bg, title_text)
        + QString("QListWidget { background: %1; border: 1px solid %2; }")
            .arg(panel, border)
        + QString("QListWidget::item:selected { background: %1; color: %2; }")
            .arg(accent, accent_text));
    cluster_group_->setStyleSheet(
      QString("QGroupBox#clusterGroup::title { subcontrol-origin: margin; left: 10px; padding: 0 6px; color: %1; background: %2; }")
      .arg(title_text, title_bg)
      + QString("QCheckBox { color: %1; font-weight: bold; }").arg(text));
    if (cat_group_) {
      cat_group_->setStyleSheet(
        QString("QGroupBox#catGroup::title { subcontrol-origin: margin; left: 10px; padding: 0 6px; color: %1; background: %2; }")
        .arg(title_text, title_bg)
        + QString("QLabel { color: %1; font-weight: bold; }").arg(text));
    }
    bandmap_group_->setStyleSheet(
      QString("QGroupBox#bandmapGroup::title { subcontrol-origin: margin; left: 10px; padding: 0 6px; color: %1; background: %2; }")
      .arg(title_text, title_bg));
    if (bandmap2_group_) {
      bandmap2_group_->setStyleSheet(
        QString("QGroupBox#bandmapGroupR2::title { subcontrol-origin: margin; left: 10px; padding: 0 6px; color: %1; background: %2; }")
        .arg(title_text, title_bg));
    }
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

  QString build_fn_status_text(const AppRenderState &state) const {
    const int active_radio = state.active_radio;
    const bool is_run = active_radio == 2 ? state.radio2_run : state.radio1_run;
    const char *mode = active_radio == 2 ? state.radio2_mode : state.radio1_mode;
    const char *rst = (mode && std::strstr(mode, "CW")) ? "599" : "59";
    const char *exchange = state.contest_exchange_sent ? state.contest_exchange_sent : "";
    const char *hiscall = active_radio == 2 ? state.input_call_r2 : state.input_call_r1;

    QStringList parts;
    parts << QString("%1").arg(is_run ? "RUN" : "S&P");

    for (int fn = 1; fn <= CW_KEY_COUNT; fn++) {
      const char *tpl = cw_keys_get(&cw_key_map_, fn, is_run ? 1 : 0);
      char expanded[256] = {0};

      if (tpl && tpl[0])
        cw_keys_expand(tpl, config_effective_operator_call(), rst, exchange, hiscall,
                       expanded, sizeof(expanded));

      const QString rendered = expanded[0]
                                   ? QString::fromLatin1(expanded).simplified()
                                   : QString("-");
      parts << QString("F%1=%2").arg(fn).arg(rendered);
    }

    return parts.join(" | ");
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
    const int status_h = std::max(
      line_panel_h,
      (contest_quick_button_ ? contest_quick_button_->sizeHint().height() : 0) + 8);
    status_panel_->setFixedHeight(status_h);
    dxcc_panel_->setFixedHeight(line_panel_h);
    stats_panel_->setFixedHeight(line_panel_h);
    const int op_h = std::max(line_panel_h * 4 + 12, op_panel_->sizeHint().height() + 6);
    op_panel_->setFixedHeight(op_h);
    gap_panel_->setFixedHeight(line_panel_h / 2);
    function_panel_->setFixedHeight(line_panel_h);
  }

  void place_suggestions_panel() {
    if (!log_group_)
      return;

    const int total_cols = std::max(1, log_group_->width() / std::max(1, cell_w_));
    const int suggest_cols = std::clamp(total_cols / 3, 24, 40);
    const int suggest_rows = 14;
    const int suggest_w = cols_to_px(suggest_cols, 14);
    const int suggest_h = (suggest_rows + 2) * (cell_h_ + 2) + 8;
    suggestions_frame_->setFixedSize(suggest_w, suggest_h);

    const int x = std::max(8, log_group_->width() - suggestions_frame_->width() - 12);
    const int y = 26;
    suggestions_frame_->move(x, y);
    suggestions_frame_->raise();
  }

  void place_scp_panel() { /* removed — merged into suggestions panel */ }

  void start_scp_update_worker() {
    if (scp_update_worker_running_)
      return;
    scp_update_worker_running_ = true;
    std::thread([this]() {
      const int ok = scp_download_latest("MASTER.SCP") == 0;
      QMetaObject::invokeMethod(
          this,
          [this, ok]() {
            scp_update_worker_running_ = false;
            if (ok) {
              scp_load("MASTER.SCP");
              QMessageBox::information(this, "SCP Update",
                QString("MASTER.SCP downloaded successfully.\n%1 callsigns loaded.")
                    .arg(scp_count()));
            } else {
              QMessageBox::warning(this, "SCP Update",
                "Failed to download MASTER.SCP.\n"
                "Check your internet connection and try again.");
            }
            refresh_ui();
          },
          Qt::QueuedConnection);
    }).detach();
  }

  void refresh_log_table() {
    std::uint64_t signature = 1469598103934665603ULL;
    signature = hash_mix(signature, (std::uint64_t)qso_count);
    signature = hash_mix(signature, (std::uint64_t)qtc_bundle_count);

    const int max_rows = 200;

    /* Compute QTC counts up-front so they are available on all code paths. */
    const int qtc_start = std::max(0, qtc_bundle_count - max_rows);
    const int qtc_rows  = qtc_bundle_count - qtc_start;

    /* Show "No QSOs" placeholder only when there are no QSOs AND no QTC rows. */
    if (qso_count <= 0 && qtc_bundle_count <= 0) {
      signature = hash_mix(signature, 0xA51EU);
      if (signature == last_log_table_signature_)
        return;

      log_table_->setRowCount(1);
      auto *item = new QTableWidgetItem("No QSOs");
      item->setTextAlignment(Qt::AlignCenter);
      log_table_->setSpan(0, 0, 1, log_table_->columnCount());
      log_table_->setItem(0, 0, item);
      last_log_table_signature_ = signature;
      return;
    }

    const int start = std::max(0, qso_count - max_rows);
    const int rows = qso_count - start;
    signature = hash_mix(signature, (std::uint64_t)start);
    signature = hash_mix(signature, (std::uint64_t)rows);

    for (int row = 0; row < rows; row++) {
      const int idx = start + row;
      const QSO &q = logbook[idx];
      signature = hash_mix(signature, (std::uint64_t)idx);
      signature = hash_cstr(signature, q.date);
      signature = hash_cstr(signature, q.utc);
      signature = hash_cstr(signature, q.call);
      signature = hash_mix(signature, (std::uint64_t)q.freq);
      signature = hash_cstr(signature, q.band);
      signature = hash_cstr(signature, q.mode);
      signature = hash_cstr(signature, q.exchange_sent);
      signature = hash_cstr(signature, q.exchange_recv);
      signature = hash_cstr(signature, q.operator_mode);
      signature = hash_mix(signature, (std::uint64_t)q.points);
      signature = hash_mix(signature, (std::uint64_t)q.invalid);
    }

    for (int bi = 0; bi < qtc_rows; bi++) {
      const QTCBundle &b = qtc_bundles[qtc_start + bi];
      signature = hash_cstr(signature, b.sender_call);
      signature = hash_cstr(signature, b.receiver_call);
      signature = hash_mix(signature, (std::uint64_t)b.bundle_nr);
      signature = hash_mix(signature, (std::uint64_t)b.record_count);
      signature = hash_mix(signature, (std::uint64_t)b.sent);
    }

    if (signature == last_log_table_signature_)
      return;

    log_table_->clearSpans();
    log_table_->setRowCount(rows + qtc_rows);

    /* --- QSO rows --- */
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

    /* --- QTC bundle rows (sent = teal background, received = amber) --- */
    for (int bi = 0; bi < qtc_rows; bi++) {
      const int table_row = rows + bi;
      const QTCBundle &b  = qtc_bundles[qtc_start + bi];

      /* Colour: sent bundles in teal, received bundles in amber. */
      const QColor bg = b.sent ? QColor(180, 230, 220) : QColor(255, 235, 170);
      const QColor fg = QColor(20, 20, 20);

      auto set_qtc_item = [&](int col, const QString &text, const QString &tip = {}) {
        auto *cell = new QTableWidgetItem(text);
        cell->setTextAlignment(Qt::AlignCenter | Qt::AlignVCenter);
        cell->setBackground(bg);
        cell->setForeground(fg);
        cell->setToolTip(tip.isEmpty() ? text : tip);
        log_table_->setItem(table_row, col, cell);
      };

      /* Col 0: bundle index within visible window */
      set_qtc_item(0, QString("Q%1").arg(qtc_start + bi + 1));

      /* Col 1-2: date/time of first record (if available) */
      const char *first_date = b.record_count > 0 ? b.records[0].date : "";
      const char *first_time = b.record_count > 0 ? b.records[0].time : "";
      set_qtc_item(1, QString::fromLatin1(first_date));
      set_qtc_item(2, QString::fromLatin1(first_time));

      /* Col 3: other station (receiver when sent, sender when received) */
      const QString other = b.sent
          ? QString::fromLatin1(b.receiver_call)
          : QString::fromLatin1(b.sender_call);
      set_qtc_item(3, other);

      /* Col 4-6: mark row as QTC meta entry. */
      set_qtc_item(4, "");
      set_qtc_item(5, "");
      set_qtc_item(6, "QTC");

      /* Show first QTC record details directly in Exch S/R column. */
      const QString first_call =
          b.record_count > 0 ? QString::fromLatin1(b.records[0].call) : QString();
      const QString first_exch =
          b.record_count > 0 ? QString::fromLatin1(b.records[0].exch) : QString();
      QString exch_text = QString("%1/%2")
                              .arg(first_call.isEmpty() ? "-" : first_call)
                              .arg(first_exch.isEmpty() ? "-" : first_exch);
      if (b.record_count > 1)
        exch_text += QString(" (+%1)").arg(b.record_count - 1);

      QString detail_tip =
          QString("%1 %4, bundle %2/%3")
              .arg(b.sent ? "Sent to" : "Recv from")
              .arg(b.bundle_nr)
              .arg(b.record_count)
              .arg(other);
      for (int ri = 0; ri < b.record_count; ri++) {
        detail_tip += QString("\n%1 %2 %3")
                          .arg(QString::fromLatin1(b.records[ri].time))
                          .arg(QString::fromLatin1(b.records[ri].call))
                          .arg(QString::fromLatin1(b.records[ri].exch));
      }

      set_qtc_item(7, exch_text, detail_tip);

      /* Col 8: direction label */
      set_qtc_item(8, b.sent ? "SENT" : "RCVD");
    }

    log_table_->scrollToBottom();
    last_log_table_signature_ = signature;
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

    set_title_if_changed(cluster_group_, QString("DX Cluster [%1]").arg(status));

    std::uint64_t signature = 1469598103934665603ULL;
    signature = hash_qstring(signature, status);
    signature = hash_mix(signature, (std::uint64_t)fullscreen_mode);
    signature = hash_mix(signature, (std::uint64_t)items.size());
    for (const SpotCopy &item : items) {
      signature = hash_qstring(signature, item.time);
      signature = hash_qstring(signature, item.freq);
      signature = hash_qstring(signature, item.call);
      signature = hash_qstring(signature, item.comment);
    }

    if (signature != last_cluster_table_signature_) {
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

      last_cluster_table_signature_ = signature;
    }

    return max_scroll;
  }

  void refresh_bandmap() {
    AppRenderState state;
    app_controller_get_render_state(&state);

    refresh_bandmap_for_radio(1, state.radio1_freq_khz, bandmap_label_,
                              bandmap_list_, bandmap_freqs_, bandmap_calls_,
                              last_bandmap_signature_);

    refresh_bandmap_for_radio(2, state.radio2_freq_khz, bandmap2_label_,
                              bandmap2_list_, bandmap2_freqs_, bandmap2_calls_,
                              last_bandmap2_signature_);
  }

  void refresh_bandmap_for_radio(int radio_nr, int target_freq_khz,
                                 QLabel *label, QListWidget *list,
                                 std::vector<int> &freqs,
                                 std::vector<QString> &calls,
                                 std::uint64_t &last_signature) {
    if (!label || !list)
      return;

    constexpr int kBandmapDuplicateWindowKhz = 2;

    struct BandmapRow {
      int freq_khz = 0;
      QString call;
      QString comment;
      QString time;
    };

    char active_band[8] = {0};
    detect_band(target_freq_khz, active_band);

    std::vector<BandmapRow> rows;
    std::uint64_t signature = 1469598103934665603ULL;
    signature = hash_mix(signature, (std::uint64_t)target_freq_khz);
    signature = hash_cstr(signature, active_band);

    pthread_mutex_lock(&dxcluster_mutex);
    const int start = spot_start;
    const int count = spot_count;
    for (int i = 0; i < count; i++) {
      const int idx = (start + i) % MAX_SPOTS;
      const int freq_khz = parse_spot_khz(spots[idx].freq);
      if (freq_khz <= 0)
        continue;

      char spot_band[8] = {0};
      detect_band(freq_khz, spot_band);
      if (std::strcmp(spot_band, active_band) != 0)
        continue;

      rows.push_back(
          {freq_khz, QString::fromLatin1(spots[idx].call),
           QString::fromLatin1(spots[idx].comment),
           QString::fromLatin1(spots[idx].time)});
    }
    pthread_mutex_unlock(&dxcluster_mutex);

    std::sort(rows.begin(), rows.end(),
              [](const BandmapRow &a, const BandmapRow &b) {
                if (a.freq_khz != b.freq_khz)
                  return a.freq_khz < b.freq_khz;
                return a.call < b.call;
              });

    // Bandmap clean-up: skip near-duplicate spots for the same callsign.
    QMap<QString, std::vector<int>> kept_freqs_by_call;
    std::vector<BandmapRow> dedup_rows;
    dedup_rows.reserve(rows.size());

    for (const BandmapRow &row : rows) {
      const QString call_key = row.call.trimmed().toUpper();
      if (call_key.isEmpty())
        continue;

      bool is_near_duplicate = false;
      std::vector<int> &kept = kept_freqs_by_call[call_key];
      for (int f : kept) {
        if (std::abs(f - row.freq_khz) <= kBandmapDuplicateWindowKhz) {
          is_near_duplicate = true;
          break;
        }
      }

      if (is_near_duplicate)
        continue;

      kept.push_back(row.freq_khz);
      dedup_rows.push_back(row);
    }

    rows.swap(dedup_rows);

    for (const BandmapRow &row : rows) {
      signature = hash_mix(signature, (std::uint64_t)row.freq_khz);
      signature = hash_qstring(signature, row.call);
      signature = hash_qstring(signature, row.comment);
      signature = hash_qstring(signature, row.time);
    }

    set_text_if_changed(
        label,
        QString("Bandmap R%1 [%2] (%3)")
            .arg(radio_nr)
            .arg(QString::fromLatin1(active_band[0] ? active_band : "?"))
            .arg(rows.size()));

    if (signature != last_signature) {
      freqs.clear();
      calls.clear();
      list->clear();

      for (const BandmapRow &row : rows) {
        const QString line = QString("%1  %2  %3")
                                 .arg(row.freq_khz, 6)
                                 .arg(row.call.left(12))
                                 .arg(row.comment.left(64));
        list->addItem(line);
        freqs.push_back(row.freq_khz);
        calls.push_back(row.call);
      }

      last_signature = signature;
    }

    if (freqs.empty())
      return;

    int target_row = -1;
    if (target_freq_khz > 0) {
      for (int i = 0; i < (int)freqs.size(); i++) {
        if (freqs[(size_t)i] == target_freq_khz) {
          target_row = i;
          break;
        }
      }
    }

    if (target_row < 0) {
      int best_delta = INT_MAX;
      for (int i = 0; i < (int)freqs.size(); i++) {
        const int delta = std::abs(freqs[(size_t)i] - target_freq_khz);
        if (delta < best_delta) {
          best_delta = delta;
          target_row = i;
        }
      }
    }

    int *selected_row = radio_nr == 2 ? &bandmap2_selected_row_ : &bandmap_selected_row_;
    const int current_row = *selected_row;
    if (current_row >= 0 && current_row < (int)freqs.size()) {
      list->setCurrentRow(current_row);
      list->scrollToItem(list->item(current_row));
      return;
    }

    if (target_row >= 0) {
      list->setCurrentRow(target_row);
      list->scrollToItem(list->item(target_row));
      *selected_row = target_row;
    }
  }

  void tune_to_bandmap_row(int row, int radio_nr) {
    if (radio_nr != 2)
      radio_nr = 1;

    std::vector<int> *freqs = radio_nr == 2 ? &bandmap2_freqs_ : &bandmap_freqs_;
    std::vector<QString> *calls = radio_nr == 2 ? &bandmap2_calls_ : &bandmap_calls_;
    if (!freqs || !calls || row < 0 || row >= (int)freqs->size())
      return;

    int *selected_row = radio_nr == 2 ? &bandmap2_selected_row_ : &bandmap_selected_row_;
    *selected_row = row;

    app_controller_set_active_radio(radio_nr);

    if (row >= 0 && row < (int)calls->size()) {
      const QString call = (*calls)[(size_t)row].trimmed();
      if (!call.isEmpty())
        app_controller_set_active_call(call.toLatin1().constData());
    }

    app_controller_set_active_frequency_khz((*freqs)[(size_t)row]);
    refresh_ui();
  }

  void navigate_bandmap(int step) {
    AppRenderState state;
    app_controller_get_render_state(&state);

    int radio_nr = app_controller_get_active_radio();
    if (radio_nr != 2)
      radio_nr = 1;

    QListWidget *list = radio_nr == 2 ? bandmap2_list_ : bandmap_list_;
    std::vector<int> *freqs = radio_nr == 2 ? &bandmap2_freqs_ : &bandmap_freqs_;
    const int active_freq_khz = radio_nr == 2 ? state.radio2_freq_khz : state.radio1_freq_khz;
    int *selected_row = radio_nr == 2 ? &bandmap2_selected_row_ : &bandmap_selected_row_;

    if (!list || !freqs || freqs->empty()) {
      list = bandmap_list_;
      freqs = &bandmap_freqs_;
      radio_nr = 1;
      selected_row = &bandmap_selected_row_;
      if (!list || freqs->empty())
        return;
    }

    int row = *selected_row;
    if (row < 0)
      row = list->currentRow();
    if (row < 0) {
      if (step > 0) {
        row = (int)freqs->size() - 1;
        for (int i = 0; i < (int)freqs->size(); i++) {
          if ((*freqs)[(size_t)i] > active_freq_khz) {
            row = i;
            break;
          }
        }
      } else {
        row = 0;
        for (int i = (int)freqs->size() - 1; i >= 0; i--) {
          if ((*freqs)[(size_t)i] < active_freq_khz) {
            row = i;
            break;
          }
        }
      }
    } else {
      row += step;
    }

    row = std::max(0, std::min(row, (int)freqs->size() - 1));
    *selected_row = row;
    list->setCurrentRow(row);
    list->scrollToItem(list->item(row));
    tune_to_bandmap_row(row, radio_nr);
  }

  void refresh_suggestions() {
    AppRenderState state;
    app_controller_get_render_state(&state);
    const char *partial = state.active_radio == 2
                            ? state.input_call_r2
                            : state.input_call_r1;

    const bool has_history = call_suggestion_available && call_suggestion_count > 0;

    char scp_results[SCP_SEARCH_MAX][SCP_CALL_LEN];
    int scp_found = 0;
    if (partial && partial[0] != '\0' && strlen(partial) >= 2 && scp_count() > 0)
      scp_found = scp_search(partial, scp_results, SCP_SEARCH_MAX);

    const bool monokai = config.ui_monokai_theme != 0;
    const QString panel_bg = monokai ? QString("#3e3d32") : QString("#9be2ef");
    const QString border = monokai ? QString("#75715e") : QString("#0e6a85");
    const QString accent = monokai ? QString("#a6e22e") : QString("#f3d24f");

    if (!has_history && scp_found == 0) {
      set_visible_if_changed(suggestions_frame_, false);
      set_stylesheet_if_changed(input_panel_,
                                QString("QFrame { background: %1; border: 1px solid %2; }").arg(panel_bg, border));
      last_suggestions_signature_ = 0;
      return;
    }

    set_visible_if_changed(suggestions_frame_, true);
    place_suggestions_panel();
    set_stylesheet_if_changed(input_panel_,
      has_history ? QString("QFrame { background: %1; border: 1px solid %2; }").arg(accent, border)
                  : QString("QFrame { background: %1; border: 1px solid %2; }").arg(panel_bg, border));

    std::uint64_t signature = 1469598103934665603ULL;
    signature = hash_mix(signature, (std::uint64_t)call_suggestion_count);
    signature = hash_mix(signature, (std::uint64_t)call_suggestion_selected_index);
    for (int i = 0; i < call_suggestion_count; i++)
      signature = hash_cstr(signature, call_suggestion_matches[i]);
    signature = hash_mix(signature, (std::uint64_t)scp_found);
    if (partial)
      signature = hash_cstr(signature, partial);

    if (signature != last_suggestions_signature_) {
      suggestions_list_->clear();

      /* History items — dark yellow */
      for (int i = 0; i < call_suggestion_count; i++) {
        auto *item = new QListWidgetItem(call_suggestion_matches[i]);
        item->setForeground(QColor(0x82, 0x6a, 0x00));
        suggestions_list_->addItem(item);
      }

      /* SCP items — dark green; skip duplicates already from history */
      for (int i = 0; i < scp_found; i++) {
        bool in_history = false;
        for (int j = 0; j < call_suggestion_count; j++) {
          if (strcasecmp(scp_results[i], call_suggestion_matches[j]) == 0) {
            in_history = true;
            break;
          }
        }
        if (!in_history) {
          auto *item = new QListWidgetItem(scp_results[i]);
          item->setForeground(QColor(0x1b, 0x5e, 0x20));
          suggestions_list_->addItem(item);
        }
      }

      last_suggestions_signature_ = signature;
    }

    if (has_history && call_suggestion_selected_index >= 0 &&
        call_suggestion_selected_index < call_suggestion_count) {
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
    if (!cat_rig_combo_)
      return;

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
    if (!cat_slot_combo_)
      return;

    if (slot != CAT_SLOT_B)
      slot = CAT_SLOT_A;

    cat_slot_combo_->blockSignals(true);
    cat_slot_combo_->setCurrentIndex(slot == CAT_SLOT_B ? 1 : 0);
    cat_slot_combo_->blockSignals(false);

    populate_cat_rigs(slot);
  }

  void refresh_cat_status() {
    char status[128] = {0};
    char slot1[128] = {0};
    char slot2[128] = {0};
    cat_get_status(status, sizeof(status));
    cat_get_status_slot(CAT_SLOT_A, slot1, sizeof(slot1));
    cat_get_status_slot(CAT_SLOT_B, slot2, sizeof(slot2));

    const ContestTechnique technique = app_controller_get_contest_technique();
    const bool dual_mode = technique != CONTEST_TECH_SO1R;
    const bool connected = cat_is_connected_slot(CAT_SLOT_A) != 0 ||
                           cat_is_connected_slot(CAT_SLOT_B) != 0;

    set_text_if_changed(cat_status_label_,
              QString("CAT status: %1").arg(QString::fromLatin1(status)));
    set_text_if_changed(cat_slot1_status_label_,
              QString("R1: %1").arg(QString::fromLatin1(slot1)));
    set_text_if_changed(cat_slot2_status_label_,
              QString("R2: %1").arg(QString::fromLatin1(slot2)));
    set_visible_if_changed(cat_slot2_status_label_, dual_mode);
    const bool monokai = config.ui_monokai_theme != 0;
    const QString ok_bg = monokai ? QString("#a6e22e") : QString("#87d37c");
    const QString warn_bg = monokai ? QString("#fd971f") : QString("#f3d24f");
    const QString border = monokai ? QString("#75715e") : QString("#0e6a85");
    set_stylesheet_if_changed(cat_status_frame_, connected
      ? QString("QFrame { background: %1; border: 1px solid %2; }").arg(ok_bg, border)
      : QString("QFrame { background: %1; border: 1px solid %2; }").arg(warn_bg, border));
    refresh_cat_cw_connection_badge();
  }

  void on_cat_connect() {
    const int slot = selected_cat_slot();
    CatConnectionParams params;
    std::memset(&params, 0, sizeof(params));

    if (slot == CAT_SLOT_B) {
      params.model = config.cat2_model;
      std::snprintf(params.device, sizeof(params.device), "%s", config.cat2_device);
      params.baud_rate = config.cat2_baud;
      params.data_bits = config.cat2_data_bits;
      params.stop_bits = config.cat2_stop_bits;
      std::snprintf(params.parity, sizeof(params.parity), "%s", config.cat2_parity);
      std::snprintf(params.handshake, sizeof(params.handshake), "%s", config.cat2_handshake);
    } else {
      params.model = config.cat_model;
      std::snprintf(params.device, sizeof(params.device), "%s", config.cat_device);
      params.baud_rate = config.cat_baud;
      params.data_bits = config.cat_data_bits;
      params.stop_bits = config.cat_stop_bits;
      std::snprintf(params.parity, sizeof(params.parity), "%s", config.cat_parity);
      std::snprintf(params.handshake, sizeof(params.handshake), "%s", config.cat_handshake);
    }

    if (params.model <= 0) {
      QMessageBox::warning(this, "CAT",
                           "No CAT rig model is configured in logger.conf. Configure station settings first.");
      return;
    }

    start_cat_connect_async(slot, params, true);
  }

  QString resolve_cw_keys_path() const {
    const QString base = QApplication::applicationDirPath();
    const QString in_app_dir = base + "/cw_keys.ini";
    if (QFileInfo::exists(in_app_dir))
      return in_app_dir;
    if (QFileInfo::exists("cw_keys.ini"))
      return "cw_keys.ini";

    const QString in_source_dir = base + "/../cw_keys.ini";
    if (QFileInfo::exists(in_source_dir))
      return in_source_dir;

    return "cw_keys.ini";
  }

  void reload_cw_maps_from_disk() {
    cw_keys_path_ = resolve_cw_keys_path();
    const QByteArray file_path = cw_keys_path_.toLocal8Bit();
    cw_keys_load(&cw_key_map_, file_path.constData());
    cw_qtc_load(&cw_qtc_map_, file_path.constData());
  }

  bool save_cw_keys_file(const CwKeyMap &map) {
    if (cw_keys_path_.isEmpty())
      cw_keys_path_ = resolve_cw_keys_path();

    const QByteArray file_path = cw_keys_path_.toLocal8Bit();
    FILE *f = std::fopen(file_path.constData(), "w");
    if (!f)
      return false;

    std::fprintf(f, "[RUN]\n");
    for (int i = 0; i < CW_KEY_COUNT; i++)
      std::fprintf(f, "F%d = %s\n", i + 1, map.run[i]);

    std::fprintf(f, "\n[S&P]\n");
    for (int i = 0; i < CW_KEY_COUNT; i++)
      std::fprintf(f, "F%d = %s\n", i + 1, map.sp[i]);

    std::fprintf(f, "\n[QTC]\n");
    std::fprintf(f, "PREAMBLE = %s\n", cw_qtc_map_.preamble);
    std::fprintf(f, "RECORD = %s\n", cw_qtc_map_.record);
    std::fprintf(f, "CONFIRM = %s\n", cw_qtc_map_.confirm);
    std::fprintf(f, "REQUEST = %s\n", cw_qtc_map_.request);
    std::fclose(f);

    cw_key_map_ = map;
    return true;
  }

  void open_cw_messages_config_dialog() {
    if (!cw_messages_dialog_)
      return;
    cw_messages_dialog_->set_map(cw_key_map_);
    cw_messages_dialog_->showAndFocus();
  }

  void on_cat_disconnect() {
    cat_disconnect_slot(selected_cat_slot());
    refresh_cat_status();
  }

  void on_cat_badge_toggle(int slot) {
    if (slot != CAT_SLOT_B)
      slot = CAT_SLOT_A;

    if (cat_connect_busy_[slot])
      return;

    load_cat_slot_form(slot);
    if (cat_is_connected_slot(slot) != 0)
      on_cat_disconnect();
    else
      on_cat_connect();
  }

  void start_cat_connect_async(int slot, CatConnectionParams params,
                               bool show_error_popup) {
    if (slot != CAT_SLOT_B)
      slot = CAT_SLOT_A;

    if (cat_connect_busy_[slot])
      return;

    cat_connect_busy_[slot] = true;
    refresh_cat_status();

    std::thread([this, slot, params, show_error_popup]() {
      const int rc = cat_connect_slot(slot, &params);
      QMetaObject::invokeMethod(
          this,
          [this, slot, rc, show_error_popup]() {
            cat_connect_busy_[slot] = false;
            refresh_cat_status();
            if (rc != 0 && show_error_popup) {
              QMessageBox::warning(this, "CAT",
                                   "CAT connection failed. Check logger.conf and connection parameters.");
            }
          },
          Qt::QueuedConnection);
    }).detach();
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
    const char *hiscall = (active == 2) ? state.input_call_r2 : state.input_call_r1;

    const char *tpl = cw_keys_get(&cw_key_map_, fn, is_run ? 1 : 0);
    if (!tpl || tpl[0] == 0) {
      cw_feedback_ = QString("F%1: brak definicji CW").arg(fn);
      cw_feedback_ticks_ = 20;
      return;
    }

    char text[256];
    cw_keys_expand(tpl, config_effective_operator_call(), rst, exch, hiscall,
                   text, sizeof(text));
    if (text[0] == 0) return;

    cw_feedback_ = QString("CW: %1").arg(QString::fromLatin1(text));
    cw_feedback_ticks_ = 20;

    if (cat_is_cw_keyer_connected()) {
      cat_cw_send(text);
    } else if (cat_send_morse_slot(CAT_SLOT_A, text) != 0) {
      cw_feedback_ = QString("CW: brak keyer/CAT — %1").arg(QString::fromLatin1(text));
    }
  }

  void on_cw_connect() {
    if (config.cw_device[0] == '\0') {
      QMessageBox::warning(this, "CW Keyer",
                           "No CW keyer configuration is set in logger.conf.");
      return;
    }

    start_cw_connect_async(true);
  }

  void on_cw_disconnect() {
    cat_disconnect_cw_keyer();
    refresh_cw_keyer_status();
  }

  void on_cw_badge_toggle() {
    if (cw_connect_busy_)
      return;

    if (cat_is_cw_keyer_connected() != 0)
      on_cw_disconnect();
    else
      on_cw_connect();
  }

  void start_cw_connect_async(bool show_error_popup) {
    if (cw_connect_busy_)
      return;

    cw_connect_busy_ = true;
    refresh_cw_keyer_status();

    const QString device = QString::fromLatin1(config.cw_device);
    const QString line = QString::fromLatin1(config.cw_keyer_line);
    const int wpm = config.cw_wpm;

    std::thread([this, device, line, wpm, show_error_popup]() {
      const QByteArray dev = device.toLocal8Bit();
      const QByteArray ln = line.toLatin1();
      const int rc = cat_connect_cw_keyer(dev.constData(), ln.constData(), wpm);
      QMetaObject::invokeMethod(
          this,
          [this, rc, show_error_popup]() {
            cw_connect_busy_ = false;
            refresh_cw_keyer_status();
            if (rc != 0 && show_error_popup) {
              QMessageBox::warning(this, "CW Keyer",
                                   "Nie można otworzyć portu CW. Sprawdź ścieżkę urządzenia.");
            }
          },
          Qt::QueuedConnection);
    }).detach();
  }

  void start_auto_connect_if_configured() {
    QTimer::singleShot(0, this, [this]() {
      if (config.cat_auto_connect && config.cat_model > 0 && config.cat_device[0]) {
        CatConnectionParams p1;
        std::memset(&p1, 0, sizeof(p1));
        p1.model = config.cat_model;
        std::snprintf(p1.device, sizeof(p1.device), "%s", config.cat_device);
        p1.baud_rate = config.cat_baud;
        p1.data_bits = config.cat_data_bits;
        p1.stop_bits = config.cat_stop_bits;
        std::snprintf(p1.parity, sizeof(p1.parity), "%s", config.cat_parity);
        std::snprintf(p1.handshake, sizeof(p1.handshake), "%s", config.cat_handshake);
        start_cat_connect_async(CAT_SLOT_A, p1, false);
      }

      if (config.cw_auto_connect && config.cw_device[0])
        start_cw_connect_async(false);
    });
  }

  void refresh_cw_keyer_status() {
    char status[128] = {0};
    cat_get_cw_keyer_status(status, sizeof(status));

    const ContestTechnique technique = app_controller_get_contest_technique();
    const bool dual_mode = technique != CONTEST_TECH_SO1R;
    const QString cw_r1 = QString("CW R1: %1").arg(QString::fromLatin1(status));
    const QString cw_r2 = QString("CW R2: %1").arg(QString::fromLatin1(status));

    set_text_if_changed(cw_status_label_, cw_r1);
    set_text_if_changed(cw_status_r2_label_, cw_r2);
    set_visible_if_changed(cw_status_r2_label_, dual_mode);

    const bool connected = cat_is_cw_keyer_connected() != 0;
    set_stylesheet_if_changed(cw_status_label_, connected
        ? "QLabel { color: #175017; font-weight: bold; }"
      : "QLabel { color: #555; }");
    set_stylesheet_if_changed(cw_status_r2_label_, connected
        ? "QLabel { color: #175017; font-weight: bold; }"
      : "QLabel { color: #555; }");
    refresh_cat_cw_connection_badge();
  }

  void refresh_cat_cw_connection_badge() {
    const ContestTechnique technique = app_controller_get_contest_technique();
    const bool dual_mode = technique != CONTEST_TECH_SO1R;

    const bool cat_r1_connected = cat_is_connected_slot(CAT_SLOT_A) != 0;
    const bool cat_r2_connected = cat_is_connected_slot(CAT_SLOT_B) != 0;
    const bool cw_connected = cat_is_cw_keyer_connected() != 0;

    set_text_if_changed(cat_conn_badge_label_,
          cat_r1_connected ? "CAT R1 DISCONNECT" : "CAT R1 CONNECT");
    set_text_if_changed(cw_conn_badge_label_,
          cw_connected ? "CW R1 DISCONNECT" : "CW R1 CONNECT");
    set_text_if_changed(cat_conn_badge_r2_label_,
          cat_r2_connected ? "CAT R2 DISCONNECT" : "CAT R2 CONNECT");
    set_text_if_changed(cw_conn_badge_r2_label_,
          cw_connected ? "CW R2 DISCONNECT" : "CW R2 CONNECT");

    set_visible_if_changed(cat_conn_badge_r2_label_, dual_mode);
    set_visible_if_changed(cw_conn_badge_r2_label_, dual_mode);

    const auto badge_style = [](bool connected) {
      return connected
        ? "QLabel { background: #2e7d32; color: #ffffff; border: 1px solid #145a1d; border-radius: 8px; padding: 2px 8px; font-weight: bold; }"
        : "QLabel { background: #b71c1c; color: #ffffff; border: 1px solid #7f1212; border-radius: 8px; padding: 2px 8px; font-weight: bold; }";
    };

    set_stylesheet_if_changed(cat_conn_badge_label_, badge_style(cat_r1_connected));
    set_stylesheet_if_changed(cw_conn_badge_label_, badge_style(cw_connected));
    set_stylesheet_if_changed(cat_conn_badge_r2_label_, badge_style(cat_r2_connected));
    set_stylesheet_if_changed(cw_conn_badge_r2_label_, badge_style(cw_connected));
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

  /*
   * Pull the latest controller state into the Qt widgets.
   *
   * @return Nothing.
   */
  void refresh_ui() {
    AppRenderState state;
    app_controller_get_render_state(&state);
    const int active_freq_khz = app_controller_get_active_frequency_khz();
    set_title_if_changed(log_group_, QString("QSO Log [%1 kHz]").arg(active_freq_khz));

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

    set_text_if_changed(input_call_r1_edit_,
                        clip_cstr_to_cols(state.input_call_r1, input_call_cols_r1));
    set_text_if_changed(input_rst_r1_edit_,
                        clip_cstr_to_cols(state.input_rst_r1, input_rst_cols_r1));
    set_text_if_changed(input_call_r2_edit_,
                        clip_cstr_to_cols(state.input_call_r2, input_call_cols_r2));
    set_text_if_changed(input_rst_r2_edit_,
                        clip_cstr_to_cols(state.input_rst_r2, input_rst_cols_r2));
    if (contest_tx_exchange_edit_) {
      const QString tx_exchange =
        QString::fromLatin1(state.contest_exchange_sent ? state.contest_exchange_sent : "").trimmed();
      set_text_if_changed(contest_tx_exchange_edit_, tx_exchange);
    }

    if (export_filename_edit_) {
      const bool export_prompt_active = app_controller_export_prompt_active();
      if (export_prompt_active) {
        if (!export_filename_edit_->isVisible())
          export_filename_edit_->show();

        const char *export_text = app_controller_export_filename_text();
        const QString controller_text = QString::fromLatin1(export_text ? export_text : "");
        if (!export_filename_edit_->hasFocus() && export_filename_edit_->text() != controller_text)
          export_filename_edit_->setText(controller_text);

        if (!export_filename_edit_->hasFocus())
          export_filename_edit_->setFocus();
      } else {
        if (export_filename_edit_->isVisible())
          export_filename_edit_->hide();
        export_filename_edit_->clear();
      }
    }

    auto set_input_style = [this](QLineEdit *edit, bool active_field) {
      const bool monokai = config.ui_monokai_theme != 0;
      const QString active_bg = monokai ? QString("#a6e22e") : QString("#f3d24f");
      const QString active_text = monokai ? QString("#1b1d1e") : QString("#111111");
      const QString idle_bg = monokai ? QString("#3e3d32") : QString("#9be2ef");
      const QString idle_text = monokai ? QString("#f8f8f2") : QString("#0d2d3b");
      const QString border = monokai ? QString("#75715e") : QString("#0e6a85");
      set_stylesheet_if_changed(edit, active_field
          ? QString("QLineEdit { background: %1; border: 1px solid %2; color: %3; }")
                .arg(active_bg, border, active_text)
          : QString("QLineEdit { background: %1; border: 1px solid %2; color: %3; }")
                .arg(idle_bg, border, idle_text));
    };

    const int active_radio = app_controller_get_active_radio();
    const bool contest_entry_mode = state.contest_entry_mode;
    set_input_style(input_call_r1_edit_, active_radio == 1 && state.active_input_field_r1 == 0);
    set_input_style(input_rst_r1_edit_, active_radio == 1 && state.active_input_field_r1 == 1);
    set_input_style(input_call_r2_edit_, active_radio == 2 && state.active_input_field_r2 == 0);
    set_input_style(input_rst_r2_edit_, active_radio == 2 && state.active_input_field_r2 == 1);
    set_text_if_changed(status_label_, clip_to_cols(
      cw_feedback_ticks_ > 0
        ? (cw_feedback_ticks_--, cw_feedback_)
        : QString("Status: %1").arg(state.status ? state.status : ""),
      std::max(1, status_cols)));
    QString info_text = QString("Info: %1").arg(state.info ? state.info : "");
    const QString operator_text = QString("OP: %1").arg(
        QString::fromLatin1(config_effective_operator_call()));

    NetSyncStatus sync_state;
    std::memset(&sync_state, 0, sizeof(sync_state));
    net_sync_get_status(&sync_state);

    QString network_text;
    if (!config.net_enabled) {
      network_text = "NET: OFF | SYNC: OFF";
    } else {
      const QString station = QString::fromLatin1(
          sync_state.station_id[0] ? sync_state.station_id : config.net_station_id);
      const QString role = QString::fromLatin1(config.net_role[0] ? config.net_role : "client");
      const QString net_state = sync_state.connected ? "ONLINE" : "OFFLINE";
      const QString sync_state_text = sync_state.connected ? "OK" : "WAIT";
      network_text = QString("NET: %1 %2 %3 | SYNC: %4 pending=%5")
                         .arg(station.isEmpty() ? QString("-") : station)
                         .arg(role)
                         .arg(net_state)
                         .arg(sync_state_text)
                         .arg(sync_state.pending_outbox);
    }

    info_text = QString("%1 | %2 | %3").arg(info_text, operator_text, network_text);

    if (contest_entry_mode) {
      const QString exchange_label =
        QString::fromLatin1(state.contest_exchange_label ? state.contest_exchange_label : "Exchange");
      const QString exchange_sent =
        QString::fromLatin1(state.contest_exchange_sent ? state.contest_exchange_sent : "");
      info_text = QString("%1 | TX %2: %3").arg(info_text, exchange_label, exchange_sent);
    }
    set_text_if_changed(info_label_, clip_to_cols(info_text, std::max(1, info_cols)));
    set_text_if_changed(dxcc_label_, clip_to_cols(QString("DXCC: %1  CQ:%2 ITU:%3")
                        .arg(state.dxcc ? state.dxcc : "")
                             .arg(last_cq)
                        .arg(last_itu),
              std::max(1, dxcc_cols)));
    set_text_if_changed(stats_label_, clip_to_cols(
      [&]() -> QString {
        QString s = QString(
          "QSO:%1 DXCC:%2  CW:%3 SSB:%4 FT8:%5 FT4:%6 RTTY:%7 PSK31:%8  PTS:%9 MULT:%10 SCORE:%11")
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
            .arg(stats.contest_score);
        if (state.contest_name && state.contest_name[0])
          s.prepend(QString("[%1]  ").arg(QString::fromLatin1(state.contest_name)));
        if (state.qtc_enabled && stats.qtc_records > 0)
          s += QString("  QTC:%1 (+%2pts)")
                   .arg(stats.qtc_records)
                   .arg(stats.qtc_points);
        return s;
      }(),
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
      set_visible_if_changed(cat_slot2_status_label_, dual_mode);
      set_visible_if_changed(cw_status_r2_label_, dual_mode);

      const QString contest_exchange_label =
        QString::fromLatin1(state.contest_exchange_label ? state.contest_exchange_label : "EXCH");
      const QString contest_exchange_sent =
        QString::fromLatin1(state.contest_exchange_sent ? state.contest_exchange_sent : "");
      const QString exchange_label = contest_entry_mode
        ? QString("%1 (TX %2)").arg(contest_exchange_label,
                                     contest_exchange_sent.isEmpty() ? "-" : contest_exchange_sent)
        : "EXCH";
      set_text_if_changed(input_rst_r1_label_, exchange_label);
      set_text_if_changed(input_rst_r2_label_, exchange_label);

      set_visible_if_changed(input_r2_label_, dual_mode);
      set_visible_if_changed(input_call_r2_label_, dual_mode);
      set_visible_if_changed(input_call_r2_edit_, dual_mode);
      set_visible_if_changed(input_rst_r2_label_, dual_mode);
      set_visible_if_changed(input_rst_r2_edit_, dual_mode);

      const bool monokai = config.ui_monokai_theme != 0;
      const QString active_style = monokai ? QString("QPushButton { background: %1; color: %2; font-weight: bold; }")
                                              .arg("#a6e22e", "#1b1d1e")
                                       : QString("QPushButton { background: %1; color: %2; font-weight: bold; }")
                                              .arg("#f3d24f", "#111111");
      const QString run_style = monokai ? QString("QPushButton { background: %1; color: %2; font-weight: bold; }")
                                            .arg("#a6e22e", "#1b1d1e")
                                     : QString("QPushButton { background: %1; color: %2; font-weight: bold; }")
                                            .arg("#87d37c", "#111111");

      set_stylesheet_if_changed(radio1_focus_button_, active_radio == 1 ? active_style : QString());
      set_stylesheet_if_changed(radio2_focus_button_, active_radio == 2 ? active_style : QString());

      set_text_if_changed(run1_toggle_button_, state.radio1_run ? "R1 RUN" : "R1 S&P");
      set_text_if_changed(run2_toggle_button_, state.radio2_run ? "R2 RUN" : "R2 S&P");
      set_stylesheet_if_changed(run1_toggle_button_, state.radio1_run ? run_style : QString());
      set_stylesheet_if_changed(run2_toggle_button_, state.radio2_run ? run_style : QString());

      set_text_if_changed(radio1_label_, QString("R1 %1 kHz %2")
                     .arg(state.radio1_freq_khz)
                     .arg(state.radio1_mode ? state.radio1_mode : ""));
      set_text_if_changed(radio2_label_, QString("R2 %1 kHz %2")
                     .arg(state.radio2_freq_khz)
                     .arg(state.radio2_mode ? state.radio2_mode : ""));

    {
      const int active_radio = state.active_radio;
      const bool is_run = active_radio == 2 ? state.radio2_run : state.radio1_run;
      const char *mode = active_radio == 2 ? state.radio2_mode : state.radio1_mode;
      const char *rst  = (mode && std::strstr(mode, "CW")) ? "599" : "59";
      const char *exch = state.contest_exchange_sent ? state.contest_exchange_sent : "";
      const char *hiscall = active_radio == 2 ? state.input_call_r2 : state.input_call_r1;
      for (int i = 0; i < CW_KEY_COUNT; i++) {
        const char *tpl = cw_keys_get(&cw_key_map_, i + 1, is_run ? 1 : 0);
        char expanded[256] = {0};
        if (tpl && tpl[0])
          cw_keys_expand(tpl, config_effective_operator_call(), rst, exch, hiscall,
                         expanded, sizeof(expanded));
        const QString lbl = expanded[0]
          ? QString("F%1: %2").arg(i + 1).arg(QString::fromLatin1(expanded).simplified())
          : QString("F%1: —").arg(i + 1);
        set_text_if_changed(fn_buttons_[i], lbl);
        if (expanded[0])
          fn_buttons_[i]->setToolTip(QString::fromLatin1(expanded).simplified());
        fn_buttons_[i]->setEnabled(!cty_update_in_progress);
      }
      const bool monokai = config.ui_monokai_theme != 0;
      const QString status_bg = monokai ? QString("#504945") : QString("#0f5ea4");
      const QString button_bg = monokai ? QString("#3e3d32") : QString("#0e4a7a");
      const QString button_text = monokai ? QString("#f8f8f2") : QString("#ffe66d");
      const QString button_hover = monokai ? QString("#6a695f") : QString("#1565a0");
      const QString accent = monokai ? QString("#a6e22e") : QString("#f3d24f");
      const QString accent_text = monokai ? QString("#1b1d1e") : QString("#111111");
      const QString border = monokai ? QString("#75715e") : QString("#2a7ab0");
      set_stylesheet_if_changed(function_panel_,
        cty_update_in_progress
          ? QString("QFrame { background: %1; }")
              .arg(monokai ? "#fd971f" : "#f3d24f")
            + QString(" QPushButton { background: %1; color: %2; font-weight: bold; border: 1px solid %3; border-radius: 3px; padding: 1px 3px; }")
              .arg(monokai ? "#f9b15e" : "#e0b030", monokai ? "#1b1d1e" : "#111111", monokai ? "#75715e" : "#a07800")
            + QString(" QPushButton:disabled { color: #555; }")
          : QString("QFrame { background: %1; }")
              .arg(status_bg)
            + QString(" QPushButton { background: %1; color: %2; font-weight: bold; border: 1px solid %3; border-radius: 3px; padding: 1px 3px; }")
              .arg(button_bg, button_text, border)
            + QString(" QPushButton:hover { background: %1; }").arg(button_hover)
            + QString(" QPushButton:pressed { background: %1; color: %2; }").arg(accent, accent_text));
    }

    refresh_log_table();
    refresh_cluster_table(false, 0);
    refresh_bandmap();
    refresh_suggestions();
    refresh_cat_status();
    refresh_cw_keyer_status();

    set_visible_if_changed(log_group_, true);
    set_visible_if_changed(input_panel_, true);
    set_visible_if_changed(status_panel_, true);
    set_visible_if_changed(dxcc_panel_, true);
    set_visible_if_changed(stats_panel_, true);
    set_visible_if_changed(gap_panel_, true);
    set_visible_if_changed(cluster_group_, state.cluster_view);
    bandmap_group_->setMinimumWidth(bandmap_r1_visible_ ? 170 : 0);
    set_visible_if_changed(bandmap_group_, bandmap_r1_visible_);
    if (bandmap2_group_) {
      bandmap2_group_->setMinimumWidth((dual_mode && bandmap_r2_visible_) ? 170 : 0);
    }
    set_visible_if_changed(bandmap2_group_, dual_mode && bandmap_r2_visible_);
    if (toggle_bandmap_r1_action_) {
      toggle_bandmap_r1_action_->blockSignals(true);
      toggle_bandmap_r1_action_->setChecked(bandmap_r1_visible_);
      toggle_bandmap_r1_action_->blockSignals(false);
    }
    if (toggle_bandmap_r2_action_) {
      toggle_bandmap_r2_action_->blockSignals(true);
      toggle_bandmap_r2_action_->setChecked(bandmap_r2_visible_);
      toggle_bandmap_r2_action_->setEnabled(dual_mode);
      toggle_bandmap_r2_action_->blockSignals(false);
    }
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
  ClickableStatusLabel *cat_conn_badge_label_ = nullptr;
  ClickableStatusLabel *cat_conn_badge_r2_label_ = nullptr;
  ClickableStatusLabel *cw_conn_badge_label_ = nullptr;
  ClickableStatusLabel *cw_conn_badge_r2_label_ = nullptr;
  QLabel *info_label_ = nullptr;
  QPushButton *contest_quick_button_ = nullptr;

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
  QPushButton *contest_config_button_ = nullptr;
  QLabel *radio1_label_ = nullptr;
  QLabel *radio2_label_ = nullptr;
  QLineEdit *contest_tx_exchange_edit_ = nullptr;
  QSpinBox *cw_wpm_spin_ = nullptr;
  QLineEdit *export_filename_edit_ = nullptr;

  QFrame *gap_panel_ = nullptr;

  QGroupBox *bandmap_group_ = nullptr;
  QGroupBox *bandmap2_group_ = nullptr;
  QGroupBox *cluster_group_ = nullptr;
  QTableWidget *cluster_table_ = nullptr;
  QLabel *bandmap_label_ = nullptr;
  QLabel *bandmap2_label_ = nullptr;
  QListWidget *bandmap_list_ = nullptr;
  QListWidget *bandmap2_list_ = nullptr;
  std::vector<int> bandmap_freqs_;
  std::vector<int> bandmap2_freqs_;
  std::vector<QString> bandmap_calls_;
  std::vector<QString> bandmap2_calls_;
  int bandmap_selected_row_ = -1;
  int bandmap2_selected_row_ = -1;
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
  QPushButton *cw_connect_button_ = nullptr;
  QPushButton *cw_disconnect_button_ = nullptr;
  QLabel *cw_status_label_ = nullptr;
  QLabel *cw_status_r2_label_ = nullptr;

  QFrame *function_panel_ = nullptr;
  std::array<QPushButton *, CW_KEY_COUNT> fn_buttons_{};

  QFrame *scp_frame_ = nullptr;  /* unused — kept for linker compat */
  QListWidget *scp_list_ = nullptr;
  bool         scp_update_worker_running_ = false;

  CwKeyerDialog *cw_dialog_ = nullptr;
  CwMessagesConfigDialog *cw_messages_dialog_ = nullptr;
  QtcDialog *qtc_dialog_ = nullptr;
  QtcHistoryDialog *qtc_history_dialog_ = nullptr;
  DocumentationDialog *documentation_dialog_ = nullptr;
  QAction *toggle_bandmap_r1_action_ = nullptr;
  QAction *toggle_bandmap_r2_action_ = nullptr;
  CwKeyMap    cw_key_map_ = {};
  CwQtcMap    cw_qtc_map_ = {};
  QString cw_keys_path_;
  /* Cached contest preset list — populated once at construction to avoid
   * blocking the main thread with filesystem scans on each dialog open. */
  std::vector<std::pair<QString, QString>> contest_presets_cache_;
  QString cw_feedback_;
  int cw_feedback_ticks_ = 0;
  bool cty_update_worker_running_ = false;
  bool bandmap_r1_visible_ = true;
  bool bandmap_r2_visible_ = true;
  bool cat_connect_busy_[CAT_SLOT_COUNT] = {false, false};
  bool cw_connect_busy_ = false;

  std::uint64_t last_log_table_signature_ = 0;
  std::uint64_t last_cluster_table_signature_ = 0;
  std::uint64_t last_bandmap_signature_ = 0;
  std::uint64_t last_bandmap2_signature_ = 0;
  std::uint64_t last_suggestions_signature_ = 0;

  int cell_w_ = 8;
  int cell_h_ = 16;
};
