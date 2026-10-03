# Исследование механизма Local Passcode в Telegram Desktop

Данный документ содержит технический анализ реализации локального пароля (Local Passcode) в Telegram Desktop (commit `d8594c011756265de4385408540bd9f7c787a003`), возможностей его программной разблокировки без участия пользователя в UI, поведения при свёрнутом окне и точек интеграции для глобальных хоткеев под Windows.

---

## 1. Где проверяется введённый пасскод и снимается блокировка

В Telegram Desktop процесс проверки пароля и снятия блокировки строго разделен на два этапа:
1. **Верификация пароля и вычисление ключа** (`Window::TryPasscode`)
2. **Снятие глобального состояния блокировки и перестроение UI** (`Core::Application::unlockPasscode`)

### Точки входа из UI
* **Полноэкранный виджет блокировки окна** (`PasscodeLockWidget`):
  * Функция: `Window::PasscodeLockWidget::submit()`
  * Файл: [window_lock_widgets.cpp:279-296](file:///Telegram/SourceFiles/window/window_lock_widgets.cpp#L279-L296)
  * Логика: получает текст из `_passcode->text()`, передает в `TryPasscode(...)`. Если результат `PasscodeAttempt::Correct`, вызывает `Core::App().unlockPasscode()`, что синхронно уничтожает сам виджет блокировки.
* **Модальное окно подтверждения пароля** (`UnlockPasscodeBox`):
  * Функция: `Window::Submit()`
  * Файл: [window_unlock_passcode_box.cpp:45-75](file:///Telegram/SourceFiles/window/window_unlock_passcode_box.cpp#L45-L75)
  * Используется при открытии защищенных разделов (настройки, вызовы) в уже заблокированном клиенте.
* **Системная биометрия (Windows Hello / Touch ID)**:
  * Функция: `Window::PasscodeLockWidget::systemUnlockDone()`
  * Файл: [window_lock_widgets.cpp:251-263](file:///Telegram/SourceFiles/window/window_lock_widgets.cpp#L251-L263)
  * При успешной аутентификации Windows Hello биометрия возвращает `SystemUnlockResult::Success`, после чего клиент напрямую вызывает `Core::App().unlockPasscode()` без повторного ручного ввода текстового пароля.

### Функция проверки: `Window::TryPasscode`
* Объявление: [window_lock_widgets.h:46](file:///Telegram/SourceFiles/window/window_lock_widgets.h#L46)
* Определение: [window_lock_widgets.cpp:41-58](file:///Telegram/SourceFiles/window/window_lock_widgets.cpp#L41-L58)
* Логика работы:
  1. Проверяет `passcode.isEmpty()` -> возвращает `PasscodeAttempt::Empty`.
  2. Проверяет флуд-контроль `!passcodeCanTry()` -> возвращает `PasscodeAttempt::Flood` (без увеличения счетчика ошибок).
  3. Конвертирует строку: `const auto utf8 = passcode.toUtf8()`.
  4. Получает доступ к домену приложения: `auto &domain = Core::App().domain()`.
  5. В зависимости от того, запущен ли домен:
     * Если `domain.started()` (Runtime-блокировка): вызывает `domain.local().checkPasscode(utf8)` ([storage_domain.cpp:252-258](file:///Telegram/SourceFiles/storage/storage_domain.cpp#L252-L258)).
     * Если `!domain.started()` (Холодный старт приложения): вызывает `domain.start(utf8) == Storage::StartResult::Success` ([main_domain.cpp:66-77](file:///Telegram/SourceFiles/main/main_domain.cpp#L66-L77) -> [storage_domain.cpp:42-68](file:///Telegram/SourceFiles/storage/storage_domain.cpp#L42-L68)).
  6. Если пароль неверен: инкрементирует `cPasscodeBadTries()` и обновляет `cPasscodeLastTry(crl::now())`, возвращает `PasscodeAttempt::Wrong`.
  7. Если пароль верен: возвращает `PasscodeAttempt::Correct`.

### Снятие блокировки: `Core::Application::unlockPasscode`
* Объявление: [core/application.h:320](file:///Telegram/SourceFiles/core/application.h#L320)
* Определение: [core/application.cpp:1341-1346](file:///Telegram/SourceFiles/core/application.cpp#L1341-L1346)
* Действия:
  1. Вызывает `clearPasscodeLock()` ([core/application.cpp:1348-1351](file:///Telegram/SourceFiles/core/application.cpp#L1348-L1351)):
     * Сбрасывает счетчик ошибок: `cSetPasscodeBadTries(0)`.
     * Переключает реактивную переменную: `_passcodeLock = false`.
  2. Перебирает все активные окна:
     ```cpp
     enumerateWindows([&](not_null<Window::Controller*> w) {
         w->clearPasscodeLock();
     });
     ```
  3. В контроллере окна `Window::Controller::clearPasscodeLock()` ([window_controller.cpp:384-390](file:///Telegram/SourceFiles/window/window_controller.cpp#L384-L390)):
     * Если это первичный холодный старт (`!_id`): вызывает `showAccount(&Core::App().activeAccount())` ([window_controller.cpp:139-234](file:///Telegram/SourceFiles/window/window_controller.cpp#L139-L234)), что инициализирует аккаунт, создает `MainWidget` и удаляет экран блокировки.
     * Если окно уже инициализировано (Runtime-блокировка): вызывает `MainWindow::clearPasscodeLock()` ([mainwindow.cpp:254-277](file:///Telegram/SourceFiles/mainwindow.cpp#L254-L277)), который уничтожает объект `_passcodeLock.destroy()` и восстанавливает видимость `_main`.

---

## 2. KDF, хранение состояния «залочено / не залочено» и потребители состояния

### Превращение пасскода в ключ (KDF)
Генерация криптографического ключа локального хранилища реализована в пространстве имен `Storage::details`:
* Функция: `Storage::details::CreateLocalKey(const QByteArray &passcode, const QByteArray &salt)`
* Заголовочный файл: [storage_file_utilities.h:23](file:///Telegram/SourceFiles/storage/details/storage_file_utilities.h#L23)
* Реализация: [storage_file_utilities.cpp:302-322](file:///Telegram/SourceFiles/storage/details/storage_file_utilities.cpp#L302-L322)

**Алгоритм KDF:**
1. **Соль**: Длина соли `LocalEncryptSaltSize = 32` байта ([storage_file_utilities.h:17](file:///Telegram/SourceFiles/storage/details/storage_file_utilities.h#L17)). Генерируется криптографически стойким ГСЧ `base::RandomFill`.
2. **Предварительное хеширование (SHA-512)**:
   ```cpp
   const auto s = bytes::make_span(salt);
   const auto hash = openssl::Sha512(s, bytes::make_span(passcode), s);
   ```
   Хешируется конкатенация: `salt + passcode + salt`. Результат: 64 байта (512 бит).
3. **Число итераций**:
   * Для непустого пароля: `kStrongIterationsCount = 100'000` итераций ([storage_file_utilities.cpp:27](file:///Telegram/SourceFiles/storage/details/storage_file_utilities.cpp#L27)).
   * Для пустого пароля (если локальный код отключен): 1 итерация.
4. **PBKDF2 HMAC**:
   Вызывается функция OpenSSL `PKCS5_PBKDF2_HMAC`:
   * Password: `hash.data()` (64 байта SHA-512)
   * Salt: исходная соль `salt.data()` (32 байта)
   * Iterations: 100 000
   * PRF / Digest: `EVP_sha512()`
   * Output: 256 байт ключа для структуры `MTP::AuthKey` ([mtproto_auth_key.h](file:///Telegram/SourceFiles/mtproto/mtproto_auth_key.h)).

**Хранение и проверка ключа в `Storage::Domain`:**
* Описание класса: [storage_domain.h:29-77](file:///Telegram/SourceFiles/storage/storage_domain.h#L29-L77)
* Поля класса:
  * `MTP::AuthKeyPtr _localKey` — мастер-ключ шифрования локальной базы и настроек ([storage_domain.h:68](file:///Telegram/SourceFiles/storage/storage_domain.h#L68)).
  * `MTP::AuthKeyPtr _passcodeKey` — ключ, полученный из пасскода через KDF ([storage_domain.h:69](file:///Telegram/SourceFiles/storage/storage_domain.h#L69)).
  * `QByteArray _passcodeKeySalt` — 32-байтовая соль ([storage_domain.h:70](file:///Telegram/SourceFiles/storage/storage_domain.h#L70)).
  * `QByteArray _passcodeKeyEncrypted` — мастер-ключ `_localKey`, зашифрованный с помощью `_passcodeKey` ([storage_domain.h:71](file:///Telegram/SourceFiles/storage/storage_domain.h#L71)).
* **Проверка пароля на лету (`checkPasscode`)**:
  В [storage_domain.cpp:252-258](file:///Telegram/SourceFiles/storage/storage_domain.cpp#L252-L258):
  ```cpp
  bool Domain::checkPasscode(const QByteArray &passcode) const {
      Expects(!_passcodeKeySalt.isEmpty());
      Expects(_passcodeKey != nullptr);

      const auto checkKey = CreateLocalKey(passcode, _passcodeKeySalt);
      return checkKey->equals(_passcodeKey);
  }
  ```
  Здесь не происходит расшифровки дисковых данных — вычисляется новый ключ и сравнивается по `equals()` (побайтовое сравнение данных ключа) с эталонным `_passcodeKey`, находящимся в памяти.

### Хранение состояния «залочено / не залочено»
1. **Глобальное состояние приложения**:
   * Переменная: `rpl::variable<bool> _passcodeLock`
   * Файл: [core/application.h:471](file:///Telegram/SourceFiles/core/application.h#L471)
   * Чтение: `Core::App().passcodeLocked()` ([core/application.cpp:1353-1355](file:///Telegram/SourceFiles/core/application.cpp#L1353-L1355)).
   * Подписка на изменения (RPL-стрим): `Core::App().passcodeLockChanges()` ([core/application.h:322](file:///Telegram/SourceFiles/core/application.h#L322)).
2. **Состояние окна**:
   * Поле: `object_ptr<Window::PasscodeLockWidget> _passcodeLock`
   * Файл: [mainwindow.h:141](file:///Telegram/SourceFiles/mainwindow.h#L141)
   * Если указатель не `nullptr`, виджет блокировки активен и закрывает рабочую область окна.
3. **Наличие установленного пароля**:
   * Флаг: `bool Storage::Domain::_hasLocalPasscode` ([storage_domain.h:74](file:///Telegram/SourceFiles/storage/storage_domain.h#L74)).
   * Проверка: `Core::App().domain().local().hasLocalPasscode()` ([storage_domain.cpp:282-284](file:///Telegram/SourceFiles/storage/storage_domain.cpp#L282-L284)).

### Потребители состояния `Core::App().passcodeLocked()`
* **DWM Taskbar / Aero Peek (Windows)**:
  * [platform/win/main_window_win.cpp:221-236](file:///Telegram/SourceFiles/platform/win/main_window_win.cpp#L221-L236): при `WM_DWMSENDICONICTHUMBNAIL` и `WM_DWMSENDICONICLIVEPREVIEWBITMAP` перехватывает системный запрос миниатюры окна и заменяет её размытой/пустой заглушкой, предотвращая утечку содержимого чатов в панели задач Windows.
* **Менеджер системных уведомлений**:
  * [notifications_manager.cpp:1088-1090](file:///Telegram/SourceFiles/window/notifications_manager.cpp#L1088-L1090): скрывает имена отправителей и текст входящих сообщений во всплывающих toast-уведомлениях, показывая нейтральное «You have a new message».
* **Горячие клавиши приложения**:
  * [core/application.cpp:2020-2026](file:///Telegram/SourceFiles/core/application.cpp#L2020-L2026): проверка `Command::Lock` (`Ctrl+L`) доступна только если клиент не заблокирован.
* **Внешний интерфейс управления (IPC / External Control)**:
  * [core/external_control.cpp:229](file:///Telegram/SourceFiles/core/external_control.cpp#L229), [core/external_control.cpp:492](file:///Telegram/SourceFiles/core/external_control.cpp#L492): блокирует выполнение внешних команд смены тем и проверки состояния при активном локе.
* **Менеджер экспорта данных**:
  * [data/data_session.cpp:1801-1806](file:///Telegram/SourceFiles/data/data_session.cpp#L1801-L1806): приостанавливает диалоги экспорта до момента разблокировки клиента.

---

## 3. Возможность вызова разблокировки напрямую со строковым паролем

### Ответ: ДА, полностью возможно
Разблокировку можно вызвать напрямую, передав пароль в виде строки `QString`, минуя виджет `PasscodeLockWidget`.

### Механика прямого вызова
Для разблокировки достаточно выполнить следующую последовательность:
```cpp
const auto result = Window::TryPasscode(passcodeString);
if (result == Window::PasscodeAttempt::Correct) {
    Core::App().unlockPasscode();
}
```

### Анализ зависимостей:
1. **Отсутствие зависимости `TryPasscode` от UI:**
   * Функция `Window::TryPasscode` ([window_lock_widgets.h:46](file:///Telegram/SourceFiles/window/window_lock_widgets.h#L46), [window_lock_widgets.cpp:41-58](file:///Telegram/SourceFiles/window/window_lock_widgets.cpp#L41-L58)) является чистой бизнес-логикой.
   * Она не принимает и не использует указатели на виджеты или окна, оперируя исключительно `QString`, `Storage::Domain` и глобальными счетчиками попыток в `settings.h`.
2. **Поведение `Core::App().unlockPasscode()`:**
   * При вызове `Core::App().unlockPasscode()` переключается `_passcodeLock = false`, и для каждого контроллера окна вызывается `w->clearPasscodeLock()`.
   * В [mainwindow.cpp:257-259](file:///Telegram/SourceFiles/mainwindow.cpp#L257-L259):
     ```cpp
     if (!_passcodeLock) {
         return;
     }
     ```
     Если виджет `_passcodeLock` отсутствовал (или уже был уничтожен), код корректно выходит без ошибок. Если виджет существовал, он уничтожается `_passcodeLock.destroy()`.
3. **Реактивные сигналы:**
   * Снятие блокировки синхронно рассылает событие через `passcodeLockChanges()`. Все вторичные модальные окна (например, `UnlockPasscodeBox` при звонках или настройках) имеют подписку на этот поток ([window_unlock_passcode_box.cpp:89-92](file:///Telegram/SourceFiles/window/window_unlock_passcode_box.cpp#L89-L92)) и автоматически закрываются при наступлении события `false`.
4. **Ограничение по потоку выполнения (Thread affinity):**
   * Вызовы `TryPasscode` и `Core::App().unlockPasscode()` **обязаны происходить в главном GUI-потоке** (Main Thread), так как они модифицируют объекты Qt и обращаются к хранилищу домена. При вызове из фонового потока необходима диспетчеризация через `crl::on_main(...)`.

---

## 4. Поведение UI после разблокировки: нужен ли показ окна

### Ответ: Показ окна НЕ нужен. Разблокировка в трее полностью валидна

### Детальный анализ состояния окна при разблокировке:
1. **Что происходит при разблокировке окна:**
   В [mainwindow.cpp:254-277](file:///Telegram/SourceFiles/mainwindow.cpp#L254-L277):
   ```cpp
   void MainWindow::clearPasscodeLock() {
       Expects(_intro || _main);
       if (!_passcodeLock) return;

       auto oldContentCache = grabForSlideAnimation();
       _passcodeLock.destroy();
       ...
       } else if (_main) {
           _main->show();
           updateControlsGeometry();
           _main->showAnimated(std::move(oldContentCache), true);
           Core::App().checkStartUrls();
       }
   }
   ```
2. **Различие между `_main->show()` и показом окна:**
   * `_main` (`MainWidget`) — это дочерний виджет контейнера `bodyWidget()`, принадлежащего `MainWindow` (наследнику `QMainWindow` / `RpWidget`).
   * В модели Qt вызов `childWidget->show()` выставляет флаг видимости дочернего элемента внутри иерархии, но **не делает видимым родительское окно верхнего уровня**, если оно скрыто (`isHidden() == true`).
   * Для отображения окна из трея в коде tdesktop существует отдельный метод `MainWindow::showFromTray()` ([main_window.cpp:621-628](file:///Telegram/SourceFiles/window/main_window.cpp#L621-L628)), который вызывает `activate()` -> `setVisible(true)` -> `activateWindow()`.
   * **Ни `clearPasscodeLock()`, ни `unlockPasscode()` метод `showFromTray()` не вызывают.**
3. **Поведение анимации `showAnimated`:**
   * Метод `_main->showAnimated(...)` запускает внутренний таймер плавного появления через `Window::SlideAnimation` ([mainwidget.cpp:2522-2537](file:///Telegram/SourceFiles/mainwidget.cpp#L2522-L2537)). Если само окно скрыто, отрисовка происходит в оффскрин-буфер, и анимация завершается вызовом `showFinished()`, не вызывая артефактов и не поднимая окно на экран.
4. **Холодный старт в трее:**
   * Если приложение было запущено в трей (с ключом `-startintray` или через автозагрузку), `MainWindow` остается скрытым. Разблокировка через KDF загружает аккаунты в память, создает сессию и переводит клиент в рабочий режим, оставляя его свёрнутым в область уведомлений.

---

## 5. Точка интеграции глобального хоткея под Windows (`RegisterHotKey` + `WM_HOTKEY`)

В архитектуре Telegram Desktop все платформо-зависимые интерфейсы вынесены в `Telegram/SourceFiles/platform/`. Для Windows существует централизованный класс системной интеграции.

### Рекомендуемое место: `Platform::WindowsIntegration`
* **Заголовочный файл**: [platform/win/integration_win.h:20-35](file:///Telegram/SourceFiles/platform/win/integration_win.h#L20-L35)
* **Файл реализации**: [platform/win/integration_win.cpp:33-68](file:///Telegram/SourceFiles/platform/win/integration_win.cpp#L33-L68), [platform/win/integration_win.cpp:156-217](file:///Telegram/SourceFiles/platform/win/integration_win.cpp#L156-L217)

### Почему именно здесь:
1. **Уже реализован `QAbstractNativeEventFilter`**:
   Класс `WindowsIntegration` наследуется от `QAbstractNativeEventFilter` ([integration_win.h:22](file:///Telegram/SourceFiles/platform/win/integration_win.h#L22)) и регистрирует себя в приложении в `WindowsIntegration::init()` ([integration_win.cpp:41](file:///Telegram/SourceFiles/platform/win/integration_win.cpp#L41)):
   ```cpp
   QCoreApplication::instance()->installNativeEventFilter(this);
   ```
2. **Глобальный перехват сообщений очереди Win32**:
   В [integration_win.cpp:55-67](file:///Telegram/SourceFiles/platform/win/integration_win.cpp#L55-L67):
   ```cpp
   bool WindowsIntegration::nativeEventFilter(
           const QByteArray &eventType,
           void *message,
           native_event_filter_result *result) {
       return Core::Sandbox::Instance().customEnterFromEventLoop([&] {
           const auto msg = static_cast<MSG*>(message);
           return processEvent(
               msg->hwnd,
               msg->message,
               msg->wParam,
               msg->lParam,
               (LRESULT*)result);
       });
   }
   ```
   События транслируются через песочницу `Core::Sandbox`, что гарантирует перехват исключений и корректную работу стека вызовов.
3. **Поддержка потоковых хоткеев без HWND**:
   При вызове Win32 API `RegisterHotKey(NULL, hotkeyId, fsModifiers, vk)` сообщение `WM_HOTKEY` кладется в очередь сообщений потока с дескриптором `msg->hwnd == NULL`.
   Метод `WindowsIntegration::processEvent` получает **все** сообщения потока независимо от наличия или валидности HWND окна.
4. **Существующий диспетчер сообщений**:
   В методе `WindowsIntegration::processEvent` ([integration_win.cpp:176-216](file:///Telegram/SourceFiles/platform/win/integration_win.cpp#L176-L216)) уже расположен `switch (msg)` для системных сообщений (`WM_COMMAND`, `WM_ENDSESSION`, `WM_TIMECHANGE`, `WM_WTSSESSION_CHANGE`, `WM_SETTINGCHANGE`). Обработка `WM_HOTKEY` ложится туда без архитектурных искажений.

### Альтернативные (менее подходящие) места:
* `Platform::MainWindow::EventFilter` ([platform/win/main_window_win.cpp:74-93](file:///Telegram/SourceFiles/platform/win/main_window_win.cpp#L74-L93)): фильтрует только сообщения, привязанные к конкретному `_window->psHwnd()`. Если окно пересоздается или закрыто в трей с уничтожением дескриптора, фильтр теряет работоспособность.
* `MainWindow::nativeEvent` ([platform/win/main_window_win.cpp:571-584](file:///Telegram/SourceFiles/platform/win/main_window_win.cpp#L571-L584)): переопределение виртуального метода `QWidget`. Не видит сообщения с `hwnd == NULL` и не вызывается при неактивном окне.

---

## 6. Поведение клиента при неверном пароле: лимиты, задержки и защита хоткея

Поведение при вводе неверного пароля регламентируется логикой флуд-контроля в `Telegram/SourceFiles/settings.h` и `window/window_lock_widgets.cpp`.

### Счётчики и временные метки
В [settings.h:105-106](file:///Telegram/SourceFiles/settings.h#L105-L106) объявлены глобальные параметры сессии:
```cpp
DeclareSetting(int32, PasscodeBadTries);
DeclareSetting(crl::time, PasscodeLastTry);
```
* `cPasscodeBadTries()` — текущее число неверных попыток (хранится в оперативной памяти в рамках сессии, не сохраняется на диск при перезапуске).
* `cPasscodeLastTry()` — временная метка последней попытки в миллисекундах (`crl::now()`).

### Таблица задержек (Rate Limiting)
Функция проверки доступности попытки `passcodeCanTry()` определена в [settings.h:112-123](file:///Telegram/SourceFiles/settings.h#L112-L123):
```cpp
inline bool passcodeCanTry() {
    if (cPasscodeBadTries() < 3) return true;
    auto dt = crl::now() - cPasscodeLastTry();
    switch (cPasscodeBadTries()) {
    case 3: return dt >= 5000;
    case 4: return dt >= 10000;
    case 5: return dt >= 15000;
    case 6: return dt >= 20000;
    case 7: return dt >= 25000;
    }
    return dt >= 30000;
}
```

| Неверных попыток (`cPasscodeBadTries`) | Необходимая задержка (`dt`) | Поведение |
| :--- | :--- | :--- |
| **0 — 2** | 0 секунд | Попытка выполняется мгновенно |
| **3** | 5 секунд | Блокировка на 5 с |
| **4** | 10 секунд | Блокировка на 10 с |
| **5** | 15 секунд | Блокировка на 15 с |
| **6** | 20 секунд | Блокировка на 20 с |
| **7** | 25 секунд | Блокировка на 25 с |
| **8 и более** | 30 секунд (максимум) | Блокировка на 30 с на каждую последующую попытку |

### Поведение при попытках
1. **При превышении лимита по времени (`!passcodeCanTry()`):**
   * В [window_lock_widgets.cpp:44-46](file:///Telegram/SourceFiles/window/window_lock_widgets.cpp#L44-L46) `TryPasscode` немедленно возвращает `PasscodeAttempt::Flood`.
   * **Важно:** при этом `cPasscodeBadTries` **не увеличивается**, и время `cPasscodeLastTry` **не сдвигается**. Спам хоткеем во время кулдауна не накапливает штраф.
2. **При ошибочном пароле:**
   * В [window_lock_widgets.cpp:53-54](file:///Telegram/SourceFiles/window/window_lock_widgets.cpp#L53-L54):
     ```cpp
     cSetPasscodeBadTries(cPasscodeBadTries() + 1);
     cSetPasscodeLastTry(crl::now());
     ```
3. **При успешном вводе пароля:**
   * В `Core::Application::clearPasscodeLock()` ([core/application.cpp:1349](file:///Telegram/SourceFiles/core/application.cpp#L1349)):
     ```cpp
     cSetPasscodeBadTries(0);
     ```
     Счетчик ошибок полностью сбрасывается в 0.
4. **Удаление данных:**
   * В коде Telegram Desktop **отсутствует** механизм автоматического удаления сессии или локальной базы данных при N неудачных попытках ввода пароля (самоуничтожение отсутствует, действует только временной троттлинг).

### Требования к реализации хоткея:
Чтобы хоткей не приводил к блокировке:
* Перед вызовом проверки проверять предикат `passcodeCanTry()`. Если он возвращает `false`, прерывать обработку без отправки попытки.
* Исключить автоматические циклические повторы при ошибке аутентификации.

---

## Схема вызовов разблокировки (Call Chain)

```
[Пользователь / Хоткей / Win32 WM_HOTKEY]
           │
           ▼
Window::TryPasscode(passcode)                     [window_lock_widgets.cpp:41]
  ├── Проверка: passcode.isEmpty()
  ├── Проверка: !passcodeCanTry()                 [settings.h:112]
  │
  ├── Вариант А: Домен уже запущен (Runtime Lock)
  │     └── Storage::Domain::checkPasscode(utf8)  [storage_domain.cpp:252]
  │           ├── Storage::details::CreateLocalKey(passcode, salt) [storage_file_utilities.cpp:302]
  │           │     ├── SHA-512(salt + passcode + salt)
  │           │     └── PKCS5_PBKDF2_HMAC(100'000 iter, SHA-512)
  │           └── checkKey->equals(_passcodeKey)
  │
  └── Вариант Б: Холодный старт (Domain Not Started)
        └── Storage::Domain::start(utf8)          [storage_domain.cpp:42]
              └── Storage::Domain::startModern()  [storage_domain.cpp:123]
                    ├── Чтение файла accounts info
                    ├── CreateLocalKey(passcode, salt)
                    ├── DecryptLocal(keyEncrypted, _passcodeKey) -> _localKey
                    ├── DecryptLocal(infoEncrypted, _localKey)
                    └── Main::Domain::activateFromStorage()

           │ (Если результат == PasscodeAttempt::Correct)
           ▼
Core::Application::unlockPasscode()               [core/application.cpp:1341]
  │
  ├── Core::Application::clearPasscodeLock()      [core/application.cpp:1348]
  │     ├── cSetPasscodeBadTries(0)               [settings.h:105]
  │     └── _passcodeLock = false                 [application.h:471]
  │           └── Уведомление подписчиков:
  │                 ├── UnlockPasscodeBox::Finish() (автозакрытие модалок)
  │                 └── Восстановление превью в панели задач Windows
  │
  └── enumerateWindows(...)                       [core/application.cpp:1343]
        └── Window::Controller::clearPasscodeLock() [window_controller.cpp:384]
              │
              ├── При холодном стар (!id):
              │     └── Controller::showAccount() -> MainWindow::setupMain()
              │
              └── При Runtime Lock (окно создано):
                    └── MainWindow::clearPasscodeLock() [mainwindow.cpp:254]
                          ├── _passcodeLock.destroy() (удаление оверлея ввода)
                          └── _main->showAnimated()   (восстановление интерфейса без активации окна)
```

---

## Оценка объёма патча

Для добавления возможности программной разблокировки по глобальному хоткею:

* **Затрагиваемые файлы**: 2–3 файла
  1. `Telegram/SourceFiles/platform/win/integration_win.h` — объявление идентификатора хоткея и методов регистрации/дерегистрации (~5–10 строк).
  2. `Telegram/SourceFiles/platform/win/integration_win.cpp` — вызовы Win32 `RegisterHotKey` / `UnregisterHotKey`, ветка `case WM_HOTKEY` в `WindowsIntegration::processEvent`, проверка `passcodeCanTry()`, вызовы `Window::TryPasscode()` и `Core::App().unlockPasscode()` (~35–50 строк).
  3. *(Опционально)* `Telegram/SourceFiles/core/core_settings.h` / `.cpp` — если требуется сохранять конфигурацию комбинации клавиш в настройки (~15–20 строк).
* **Примерное число строк кода**: **40 – 70 строк**.
* **Уровень риска**: **НИЗКИЙ (Low)**.
  * **Обоснование**:
    * Не требуется изменять внутренности криптографии (`storage_file_utilities.cpp`), формат локальной базы или протокольные структуры.
    * Функции `Window::TryPasscode` и `Core::App().unlockPasscode()` уже публично экспортированы в заголовочных файлах `window/window_lock_widgets.h` и `core/application.h`, обладая полностью автономной семантикой.
    * Не нарушается жизненный цикл оконного менеджера и Qt-виджетов.
    * Все вызовы из нативного фильтра событий `WindowsIntegration` автоматически синхронизированы с главным циклом событий через `Core::Sandbox::customEnterFromEventLoop`.

---

## Вердикт

**«Да, просто»** (с разделением логики для Runtime Lock и Cold Start).

1. **При Runtime Lock (клиент уже запущен и свёрнут в трей):**
   Прямой вызов пары `Window::TryPasscode(passcode)` и `Core::App().unlockPasscode()` полностью разблокирует клиент, сбрасывает счетчики, разблокирует уведомления и DWM-превью. Окно клиента при этом **не отображается на экране и не забирает фокус у пользователя**, оставаясь в трее.
2. **При Cold Start (клиент только что запущен):**
   Вызов `TryPasscode` инициализирует `Storage::Domain`, расшифровывает ключи аккаунтов и загружает сессию. Если клиент запускался в трей (параметр `-startintray`), окно останется в трее в уже расшифрованном состоянии.

---

## Места возможной ломки патча обновлениями Upstream

1. **Сигнатура и расположение функции `TryPasscode`:**
   * Файлы: [window/window_lock_widgets.h:46](file:///Telegram/SourceFiles/window/window_lock_widgets.h#L46), [window/window_lock_widgets.cpp:41](file:///Telegram/SourceFiles/window/window_lock_widgets.cpp#L41)
   * Риск: Рефакторинг пространства имен `Window` или перенос логики авторизации в подсистему `Storage` / `Core` или перевод KDF на асинхронные корутины (`crl::async`).
2. **Платформенная интеграция Windows:**
   * Файлы: [platform/win/integration_win.h](file:///Telegram/SourceFiles/platform/win/integration_win.h), [platform/win/integration_win.cpp](file:///Telegram/SourceFiles/platform/win/integration_win.cpp)
   * Риск: Обновления версии Qt (переход между минорными версиями Qt 6) и изменения в приватных интерфейсах `QWindowsApplication` / `QAbstractNativeEventFilter`.
3. **Метод `MainWindow::clearPasscodeLock`:**
   * Файл: [mainwindow.cpp:254-277](file:///Telegram/SourceFiles/mainwindow.cpp#L254-L277)
   * Риск: Появление явного принудительного вызова `activate()` или `show()` внутри метода очистки блокировки в будущих коммитах tdesktop.
4. **Константы времени и счетчиков в `settings.h`:**
   * Файл: [settings.h:105-123](file:///Telegram/SourceFiles/settings.h#L105-L123)
   * Риск: Перенос настроек флуд-контроля в структурированное хранилище `Core::Settings` с удалением макросов `DeclareSetting(..., PasscodeBadTries)`.
