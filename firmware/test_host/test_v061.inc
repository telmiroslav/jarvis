// Тесты v0.6.1: сторож запуска слова смотрит на причину сброса (mww_logic.h: ResetKind, reset_is_crash, guard_action).
// Подключается из test_main.cc (там макросы CHECK и список проверок в main()).

// ---------------------------------------------------------------- какие сбросы считаются падением

static void test_reset_kind_v061()
{
    using namespace mww_logic;
    // Падение программы: панике, трём видам сторожевых таймеров и CPU_LOCKUP
    CHECK(reset_is_crash(ResetKind::Panic));
    CHECK(reset_is_crash(ResetKind::IntWdt));
    CHECK(reset_is_crash(ResetKind::TaskWdt));
    CHECK(reset_is_crash(ResetKind::Wdt));
    CHECK(reset_is_crash(ResetKind::CpuLockup));
    // Не падение: питание, кнопка, esp_restart (reboot), сон, USB (монитор, перепрошивка, ноутбук), отладчик, eFuse и неизвестное
    CHECK(!reset_is_crash(ResetKind::PowerOn));
    CHECK(!reset_is_crash(ResetKind::Ext));
    CHECK(!reset_is_crash(ResetKind::Sw));
    CHECK(!reset_is_crash(ResetKind::DeepSleep));
    CHECK(!reset_is_crash(ResetKind::Brownout));
    CHECK(!reset_is_crash(ResetKind::Sdio));
    CHECK(!reset_is_crash(ResetKind::Usb));
    CHECK(!reset_is_crash(ResetKind::Jtag));
    CHECK(!reset_is_crash(ResetKind::Efuse));
    CHECK(!reset_is_crash(ResetKind::PwrGlitch));
    CHECK(!reset_is_crash(ResetKind::Unknown));
    // Питание: просадка и сбой питания (не падение, но с предупреждением в лог), больше ничто
    CHECK(reset_is_power(ResetKind::Brownout));
    CHECK(reset_is_power(ResetKind::PwrGlitch));
    CHECK(!reset_is_power(ResetKind::Panic));
    CHECK(!reset_is_power(ResetKind::PowerOn));    // включение питания это не его потеря посреди работы
    CHECK(!reset_is_power(ResetKind::Usb));
    CHECK(!reset_is_power(ResetKind::Unknown));
    // Полный обход enum: ровно 5 падений, 2 питания (оба не падения) и все 16 значений различны (новую причину в enum придётся отнести куда-то в этом тесте)
    int crashes = 0, power = 0, total = 0;
    for (int i = 0; i <= static_cast<int>(ResetKind::CpuLockup); i++) {
        const ResetKind k = static_cast<ResetKind>(i);
        total++;
        crashes += reset_is_crash(k) ? 1 : 0;
        power += reset_is_power(k) ? 1 : 0;
        CHECK(!(reset_is_crash(k) && reset_is_power(k)));
    }
    CHECK(total == 16 && crashes == 5 && power == 2);
    // Мусор (значение вне enum) не падение
    CHECK(!reset_is_crash(static_cast<ResetKind>(200)) && !reset_is_power(static_cast<ResetKind>(200)));
}

// ---------------------------------------------------------------- guard_action

static void test_guard_v061()
{
    using namespace mww_logic;
    const Engine engines[] = {Engine::Mww, Engine::Esp};
    for (const Engine e : engines) {
        // сторож не стоял: всегда запускаем, какой бы ни была причина сброса (в том числе падение: это не из-за движка)
        for (int i = 0; i <= static_cast<int>(ResetKind::CpuLockup); i++) {
            CHECK(guard_action(e, false, static_cast<ResetKind>(i)) == GuardAction::Proceed);
        }
        // стоял и прошлый запуск упал: прежнее поведение (mww переходит на ESP-SR, esp в этом запуске без слова)
        const ResetKind crashes[] = {ResetKind::Panic, ResetKind::IntWdt, ResetKind::TaskWdt, ResetKind::Wdt, ResetKind::CpuLockup};
        for (const ResetKind k : crashes) {
            CHECK(guard_action(e, true, k) == (e == Engine::Mww ? GuardAction::DemoteToEsp : GuardAction::Fail));
        }
        // стоял, но прошлый запуск кончился не падением: запускаем как обычно, ничего не выключаем и ни на что не переходим
        const ResetKind not_crashes[] = {ResetKind::PowerOn, ResetKind::Ext, ResetKind::Sw, ResetKind::DeepSleep, ResetKind::Brownout, ResetKind::Sdio,
                                         ResetKind::Usb, ResetKind::Jtag, ResetKind::Efuse, ResetKind::PwrGlitch, ResetKind::Unknown};
        for (const ResetKind k : not_crashes) {
            CHECK(guard_action(e, true, k) == GuardAction::Proceed);
        }
    }
    // те же случаи в явном виде: {сторож нет, падение, не падение} x {mww, esp}
    CHECK(guard_action(Engine::Mww, false, ResetKind::Usb) == GuardAction::Proceed);
    CHECK(guard_action(Engine::Esp, false, ResetKind::Usb) == GuardAction::Proceed);
    CHECK(guard_action(Engine::Mww, true, ResetKind::Panic) == GuardAction::DemoteToEsp);
    CHECK(guard_action(Engine::Esp, true, ResetKind::Panic) == GuardAction::Fail);
    CHECK(guard_action(Engine::Mww, true, ResetKind::Usb) == GuardAction::Proceed);
    CHECK(guard_action(Engine::Esp, true, ResetKind::Usb) == GuardAction::Proceed);
}

// ---------------------------------------------------------------- несколько загрузок подряд

namespace {

// Загрузка платы так, как её ведёт wake::init: флаг сторожа в NVS и сохранённый выбор движка. Сами решения берутся у guard_action,
// остальное (кто и когда ставит и снимает сторож) повторяет wake.cc: Proceed и DemoteToEsp ставят сторож перед запуском движка, Demote и Fail
// снимают прежний, а через 30 с работы движка его снимает задача.
struct GuardBoard {
    bool guard = false;
    mww_logic::Engine saved = mww_logic::Engine::Mww;
    bool started = false;       // движок в этом запуске стартовал
    bool skipped = false;       // слова в этом запуске нет (Fail)
    bool demoted = false;       // в этом запуске выбор mww сохранён как esp

    void boot(mww_logic::ResetKind prev)
    {
        using mww_logic::GuardAction;
        started = skipped = demoted = false;
        switch (mww_logic::guard_action(saved, guard, prev)) {
        case GuardAction::DemoteToEsp:
            saved = mww_logic::Engine::Esp;
            guard = false;
            demoted = true;
            break;
        case GuardAction::Fail:
            guard = false;       // движок в этом запуске не работает: чем бы он ни кончился, это не вина движка
            skipped = true;
            return;
        case GuardAction::Proceed:
            break;
        }
        guard = true;            // перед запуском движка
        started = true;
    }
    // Плата проработала s секунд и сброшена
    void run(int s)
    {
        if (started && s >= 30) {
            guard = false;
        }
    }
};

}  // namespace

static void test_guard_boot_sequences_v061()
{
    using mww_logic::Engine;
    using mww_logic::ResetKind;
    // Случай хозяина платы: два обычных сброса подряд в первые 30 с (кабель, reboot, монитор, перепрошивка) с каждым из движков
    const Engine engines[] = {Engine::Mww, Engine::Esp};
    const ResetKind plain[] = {ResetKind::Usb, ResetKind::Sw, ResetKind::Ext, ResetKind::PowerOn};
    for (const Engine e : engines) {
        for (const ResetKind r1 : plain) {
            for (const ResetKind r2 : plain) {
                GuardBoard b;
                b.saved = e;
                b.boot(ResetKind::PowerOn);
                b.run(10);
                b.boot(r1);
                CHECK(b.started && !b.skipped && !b.demoted && b.saved == e);
                b.run(3);
                b.boot(r2);
                CHECK(b.started && !b.skipped && !b.demoted && b.saved == e);    // выбранная модель на месте, слово не выключено
                b.run(30);
                CHECK(!b.guard);
            }
        }
    }
    // Просадка питания в первые 30 с: не падение (в лог идёт предупреждение), движок и выбор не трогаем
    {
        GuardBoard b;
        b.boot(ResetKind::PowerOn);
        b.run(5);
        b.boot(ResetKind::Brownout);
        CHECK(b.started && !b.demoted && b.saved == Engine::Mww);
        b.run(5);
        b.boot(ResetKind::PwrGlitch);
        CHECK(b.started && !b.demoted && b.saved == Engine::Mww);
    }
    // Настоящее падение при запуске с mww: переход на ESP-SR и сохранение; сторож снят, движок ESP-SR запущен со свежим сторожем
    {
        GuardBoard b;
        b.boot(ResetKind::PowerOn);
        b.run(2);
        b.boot(ResetKind::Panic);
        CHECK(b.demoted && b.started && !b.skipped && b.saved == Engine::Esp && b.guard);
        b.run(30);
        CHECK(!b.guard);
        b.boot(ResetKind::Usb);            // дальше обычный запуск на ESP-SR
        CHECK(b.started && b.saved == Engine::Esp && !b.demoted);
    }
    // Падение при запуске с esp: в этом запуске слова нет, и чем бы он ни кончился, следующая загрузка пробует движок снова
    const ResetKind enders[] = {ResetKind::Usb, ResetKind::Sw, ResetKind::PowerOn, ResetKind::TaskWdt, ResetKind::Panic};
    for (const ResetKind ender : enders) {
        GuardBoard b;
        b.saved = Engine::Esp;
        b.boot(ResetKind::PowerOn);
        b.run(4);
        b.boot(ResetKind::IntWdt);
        CHECK(b.skipped && !b.started && b.saved == Engine::Esp && !b.guard);
        b.run(1000);                        // слова нет, а плата работает (кнопка BOOT)
        b.boot(ender);
        CHECK(b.started && !b.skipped && b.saved == Engine::Esp);    // движок попробовал снова сам, без команды wake on
    }
    // Если и повторный запуск падает, это снова ловится: ещё одна загрузка с падением даёт Fail
    {
        GuardBoard b;
        b.saved = Engine::Esp;
        b.boot(ResetKind::PowerOn);
        b.boot(ResetKind::Panic);
        CHECK(b.skipped);
        b.boot(ResetKind::Usb);
        CHECK(b.started);
        b.run(3);
        b.boot(ResetKind::Panic);
        CHECK(b.skipped);
    }
    // Движок прожил 30 с (сторож снят): последующее падение по любой причине не трогает ни выбор, ни движок
    {
        GuardBoard b;
        b.boot(ResetKind::PowerOn);
        b.run(31);
        b.boot(ResetKind::Panic);
        CHECK(b.started && !b.demoted && !b.skipped && b.saved == Engine::Mww);
    }
}
