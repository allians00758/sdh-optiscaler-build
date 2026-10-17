# SDH OptiScaler 10 — GitHub Actions build (Live API v2)

**Статус: экспериментальная сборка для тестирования.** Этот проект создаёт **Windows x64 `dxgi.dll` OptiScaler** из официальных исходников ветки `plugin-api` с исходным патчем SDH Live API v2. Он **не содержит готовой пересобранной `dxgi.dll`** до запуска workflow.

## Как запустить

1. Создай **пустой репозиторий** GitHub (желательно приватный, пока мы тестируем). Название, например, `sdh-optiscaler-build`.
2. Распакуй ZIP проекта и загрузи **его содержимое**, включая скрытую папку `.github/workflows/`, в корень репозитория. Через `git`:

   ```bash
   git clone https://github.com/ТВОЙ_АККАУНТ/sdh-optiscaler-build.git
   cd sdh-optiscaler-build
   # Скопируй сюда содержимое ZIP, сохранив .github/
   git add .
   git commit -m "Add SDH OptiScaler Windows build"
   git push
   ```

3. Открой вкладку **Actions** → **Build SDH OptiScaler Live API (Windows x64)** → **Run workflow**. Оставь указанный по умолчанию коммит `f24ebc05404b1bda179b42314a06c8ab40462f20` и запусти сборку.
4. После завершения открой запуск в Actions и скачай артефакт **`SDH-OptiScaler10-LiveAPI-<коммит>`** в разделе **Artifacts**. Внутри ожидаются:

   ```text
   dxgi.dll
   sdh-optiscaler-control.asi
   BUILD-INFO.txt
   ```

5. **Не заменяй этими DLL рабочую установку сразу**. Сначала проверим сборку и логи; затем будем тестировать в отдельной закрытой игре с резервной копией. Переключение FSR4/HelixSR через символьные ссылки и остальные файлы текущей установки не изменяются этой сборкой.

## Что делает workflow

- Забирает официальный `https://github.com/optiscaler/OptiScaler.git` с сабмодулями и переключается на закреплённый коммит `plugin-api`.
- Применяет `scripts/patch_optiscaler_plugin_api.py`, добавляя версионированный API v2.
- Компилирует `OptiScaler.sln` для Windows x64 / Release с MSBuild на `windows-2022`.
- Проверяет, что DLL имеет формат Windows PE x64; убеждается, что собранная ранее companion ASI экспортирует `InitializeASI` и `OptiPluginInit`.
- Публикует **только артефакт GitHub Actions**, не создавая релиз, не обновляя Steam Deck и не перезаписывая файлы автоматически.

## Совместимость с будущими версиями

В API v2 поля добавлены **в конец существующей таблицы вызовов** — старые поля API v1 не переставляются. В ASI проверяется размер таблицы. Это снижает зависимость ASI от внутренней структуры `dxgi.dll`.

Но **оригинальные Nightly без этого расширения не обеспечивают SDH live-control**. Чтобы не пересобирать ядро при каждом обновлении OptiScaler, изменения должны попасть в официальный проект; до этого нужно отдельно собирать совместимую DLL или сохранять закреплённую версию.

Выбор другого `upstream_ref` в Actions возможен, но если структура upstream изменилась, скрипт прервёт сборку **без публикации ошибочного артефакта**.

## Известные ограничения

- Сам `dxgi.dll` этим архивом **не собран**, и GitHub Actions ещё не запускался.
- Локальные C++ тесты проверяют логику изменения `Config` на имитационных данных, не настоящий рендерер.
- `apply_stage=renderer_reinit_requested` означает **запрос**, не подтверждение завершённого пересоздания. Действительное изменение качества и масштабирования проверяется в игре.
- ASI использует `SDH_OPTISCALER_CONTROL_DIR`, предоставляемый Proton-хуком SDH FrameGen. Без корректной передачи этой переменной ASI не сможет найти канал команд.
- Это экспериментальный патч ветки `plugin-api`, не часть официального стабильного OptiScaler.

## Файлы

- `.github/workflows/build-sdh-optiscaler.yml` — сборка в GitHub Actions.
- `scripts/patch_optiscaler_plugin_api.py` — патч исходников.
- `scripts/sdh_plugin_api_extension.inc` — рендер-потоковый обработчик команд SDH.
- `scripts/verify_integration.py` — статические проверки патча.
- `scripts/verify_pe.py` — проверка Windows PE x64 / экспортов.
- `asi/sdh-optiscaler-control.asi` — ранее собранный companion ASI, **не новый `dxgi.dll`**.
- `asi/sdh-optiscaler-control.c` — исходный код этой ASI.
- `tests/test_live_provider.cpp` — имитационные тесты.

Исходная реализация OptiScaler и её зависимости остаются собственностью соответствующих правообладателей и распространяются на условиях их лицензий. Исходники SDH helper помечены SPDX GPL-3.0-or-later.
