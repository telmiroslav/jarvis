// Тесты v0.6.0: музыка на колонке (music_logic.h), эхоподавление (aec_logic.h) и их проводка (протокол, разговор, слово, лицо, команды).
// Подключается из test_main.cc (там макросы CHECK и список проверок в main()).

#include <algorithm>
#include <cmath>

#include "aec_logic.h"
#include "music_logic.h"

// ---------------------------------------------------------------- протокол: музыка

static void test_proto_music()
{
    using proto::Kind;
    bool ok = false;
    proto::ServerMsg m = parse("{\"type\":\"music_start\",\"rate\":24000,\"volume\":40,\"title\":\"Capsize \\u2014 \\u0422\\u044b \\u043b\\u044e\\u0431\\u0438\\u0448\\u044c\"}", &ok);
    CHECK(ok && m.kind == Kind::MusicStart && m.rate == 24000 && m.volume == 40);
    CHECK(strcmp(m.title, "Capsize — Ты любишь") == 0);
    // название по-русски как есть (UTF-8), любой порядок ключей
    m = parse("{\"title\":\"Кино — Группа крови\",\"volume\":55,\"type\":\"music_start\",\"rate\":24000}", &ok);
    CHECK(ok && m.kind == Kind::MusicStart && m.volume == 55 && strcmp(m.title, "Кино — Группа крови") == 0);
    // громкости нет: -1 (плата возьмёт по умолчанию)
    m = parse("{\"type\":\"music_start\",\"rate\":24000,\"title\":\"x\"}", &ok);
    CHECK(ok && m.kind == Kind::MusicStart && m.volume == -1);
    m = parse("{\"type\":\"music_start\",\"volume\":\"40\"}", &ok);   // строка вместо числа не годится
    CHECK(ok && m.volume == -1);
    m = parse("{\"type\":\"music_volume\",\"volume\":0}", &ok);
    CHECK(ok && m.kind == Kind::MusicVolume && m.volume == 0);
    m = parse("{\"type\":\"music_volume\",\"volume\":100}", &ok);
    CHECK(ok && m.kind == Kind::MusicVolume && m.volume == 100);
    m = parse("{\"type\":\"music_volume\",\"volume\":37.6}", &ok);   // дробная часть отбрасывается
    CHECK(ok && m.volume == 37);
    m = parse("{\"type\":\"music_title\",\"title\":\"Next track\"}", &ok);
    CHECK(ok && m.kind == Kind::MusicTitle && strcmp(m.title, "Next track") == 0);
    m = parse("{\"type\":\"music_pause\"}", &ok);
    CHECK(ok && m.kind == Kind::MusicPause);
    m = parse("{\"type\":\"music_resume\"}", &ok);
    CHECK(ok && m.kind == Kind::MusicResume);
    m = parse("{\"type\":\"music_stop\"}", &ok);
    CHECK(ok && m.kind == Kind::MusicStop);
    m = parse("{\"type\":\"music_end\"}", &ok);
    CHECK(ok && m.kind == Kind::MusicEnd);
    m = parse("{\"type\":\"music_dance\"}", &ok);   // незнакомое музыкальное сообщение (новее протокол): игнорируется
    CHECK(ok && m.kind == Kind::Unknown);
    // длинное название режется по границе символа
    {
        std::string big = "{\"type\":\"music_start\",\"title\":\"";
        for (int i = 0; i < 300; i++) {
            big += "Ы";
        }
        big += "\"}";
        m = parse(big.c_str(), &ok);
        CHECK(ok && m.title_truncated && strlen(m.title) <= proto::kMaxTitle && utf8_len_ok(m.title) && strlen(m.title) >= proto::kMaxTitle - 1);
    }
    // исходящее и hello
    CHECK(strcmp(proto::out_text(proto::OutMsg::MusicDone), "{\"type\":\"music_done\"}") == 0);
    char hello[256];
    proto::build_hello(hello, sizeof(hello), "v0.6.0", "10:51:db:aa:bb:cc");
    CHECK(strstr(hello, "\"caps\":[\"music\"]") != nullptr);
    CHECK(hello[strlen(hello) - 1] == '}');
    // ответ сервера старого образца на hello разбирается как раньше (caps в welcome не нужен)
    m = parse("{\"type\":\"welcome\",\"proto\":1,\"server\":\"0.5.0\",\"tts_rate\":24000}", &ok);
    CHECK(ok && m.kind == Kind::Welcome && m.proto == 1);
}

// ---------------------------------------------------------------- музыка: громкость, маршрутизация кадров, плеер

static void test_music_volume_curve()
{
    using namespace music_logic;
    CHECK(clamp_volume(-5) == 0 && clamp_volume(0) == 0 && clamp_volume(40) == 40 && clamp_volume(100) == 100 && clamp_volume(250) == 100);
    CHECK(volume_to_gain_q15(100) == 32768);
    CHECK(volume_to_gain_q15(0) == 0);
    CHECK(volume_to_gain_q15(40) == 5243);                  // (0,4)^2 * 32768
    CHECK(volume_to_gain_q15(500) == 32768 && volume_to_gain_q15(-1) == 0);
    CHECK_NEAR(gain_q15_to_db(32768), 0.0, 1e-3);
    CHECK_NEAR(gain_q15_to_db(volume_to_gain_q15(70)), -6.2, 0.1);
    CHECK_NEAR(gain_q15_to_db(volume_to_gain_q15(40)), -15.9, 0.1);
    CHECK(gain_q15_to_db(0) == -120.0f);
    // монотонно
    int32_t prev = -1;
    for (int v = 0; v <= 100; v++) {
        const int32_t g = volume_to_gain_q15(v);
        CHECK(g >= prev);
        prev = g;
    }
}

static void test_frame_router()
{
    using namespace music_logic;
    FrameRouter r;
    CHECK(r.route(false) == Route::Drop && r.route(true) == Route::Music);   // вне голоса: музыка, если режим включён, иначе выброс
    r.on_audio_start();
    CHECK(r.voice_open() && r.route(false) == Route::Voice && r.route(true) == Route::Voice);   // внутри audio_start...audio_end всегда голос
    r.on_audio_end();
    CHECK(!r.voice_open() && r.route(true) == Route::Music && r.route(false) == Route::Drop);
    // голос старого ответа после abort остаётся «голосом» до audio_end: он не должен попасть в музыку
    r.on_audio_start();
    CHECK(r.route(true) == Route::Voice);
    // music_start посреди «голоса»: audio_end потерялся, дальше музыка
    r.on_music_start();
    CHECK(!r.voice_open() && r.route(true) == Route::Music);
    r.on_audio_start();
    r.reset();   // обрыв связи
    CHECK(!r.voice_open() && r.route(true) == Route::Music);
    CHECK(strcmp(route_name(Route::Voice), "voice") == 0 && strcmp(route_name(Route::Music), "music") == 0 && strcmp(route_name(Route::Drop), "drop") == 0);
}

// Звук для проверок плеера: константа dc (так по выходу видно усиление)
static std::vector<int16_t> const_audio(size_t n, int16_t v) { return std::vector<int16_t>(n, v); }

static void test_music_player()
{
    using namespace music_logic;
    std::vector<int16_t> store(kCapacitySamples);
    Player p;
    p.init(store.data(), store.size());
    CHECK(!p.active() && !p.playing() && !p.paused() && p.state() == Player::State::Off);
    int16_t out[480];
    CHECK(p.pull(out, 480) == 0);                          // режим выключен: тишина
    CHECK(p.push(const_audio(100, 1000).data(), 100) == 0);   // и кадры не принимаются
    CHECK(!p.resume());
    p.pause(true);
    CHECK(p.state() == Player::State::Off);                // пауза выключенного ничего не делает

    // старт: буфер копится 0,2 с (4800 отсчётов), до тех пор тишина
    CHECK(p.start(100));
    CHECK(p.active() && p.playing() && !p.paused() && p.starts() == 1);
    const std::vector<int16_t> dc = const_audio(2400, 10000);
    CHECK(p.push(dc.data(), dc.size()) == 2400);
    CHECK(p.pull(out, 480) == 0);                          // ещё не накопилось
    CHECK(p.push(dc.data(), dc.size()) == 2400);           // теперь 4800: играем
    size_t got = p.pull(out, 480);
    CHECK(got == 480);
    // нарастание с нуля: первые отсчёты тихие, дальше растёт ровно на kFadeStepQ15, за ~241 отсчёт доходит до полного
    CHECK(out[0] > 0 && out[0] < 400);
    bool monotone = true;
    for (size_t i = 1; i < 241 && i < got; i++) {
        if (out[i] < out[i - 1]) {
            monotone = false;
        }
    }
    CHECK(monotone);
    CHECK(out[300] == 10000 && out[479] == 10000);         // громкость 100 = усиление 1,0: звук без изменений
    CHECK(p.gain_q15() == 32768 && p.target_gain_q15() == 32768);

    // пауза: затухание ~10 мс, берём из буфера ровно столько, сколько нужно; остальное остаётся на после паузы
    const size_t buffered_before = p.buffered();
    p.pause(true);
    CHECK(p.paused() && p.paused_by_talk() && p.state() == Player::State::Pausing && p.playing());
    CHECK(p.target_gain_q15() == 0);
    size_t fade = p.pull(out, 480);
    CHECK(fade > 200 && fade <= 242);
    CHECK(out[0] > 9000 && out[fade - 1] < 200);           // от почти полного к нулю
    monotone = true;
    for (size_t i = 1; i < fade; i++) {
        if (out[i] > out[i - 1]) {
            monotone = false;
        }
    }
    CHECK(monotone);
    CHECK(p.state() == Player::State::Paused && p.gain_q15() == 0);
    CHECK(p.buffered() == buffered_before - fade);          // потеряно только то, что проиграно при затухании
    CHECK(p.pull(out, 480) == 0);                           // на паузе тишина
    CHECK(p.buffered() == buffered_before - fade);          // и буфер держится
    // кадры продолжают копиться на паузе
    CHECK(p.push(dc.data(), 480) == 480);
    CHECK(p.buffered() == buffered_before - fade + 480);
    // продолжение: из буфера, нарастание быстрое
    CHECK(p.resume() && p.playing() && !p.paused() && !p.paused_by_talk());
    got = p.pull(out, 480);
    CHECK(got == 480 && out[0] < 200 && out[479] == 10000);
    // повторная пауза и продолжение не теряют звук: всё, что ушло, посчитано
    CHECK(p.total_out() + p.buffered() == p.total_in());

    // громкость плавно: со 100 на 10, без скачков (не быстрее kVolumeStepQ15 за отсчёт)
    p.set_volume(10);
    CHECK(p.volume() == 10);
    int32_t last_gain = 32768;
    bool smooth = true;
    for (int chunk = 0; chunk < 4; chunk++) {
        if (p.buffered() < 480) {
            p.push(dc.data(), 480);
        }
        got = p.pull(out, 480);
        for (size_t i = 0; i < got; i++) {
            const int32_t g = out[i] * 32768 / 10000;
            if (last_gain - g > kVolumeStepQ15 + 12) {   // запас на округление при пересчёте усиления из выхода
                smooth = false;
            }
            last_gain = g;
        }
    }
    CHECK(smooth);
    CHECK(p.gain_q15() == volume_to_gain_q15(10) && out[479] == 10000 * volume_to_gain_q15(10) / 32768);
    // и вверх
    p.set_volume(100);
    for (int chunk = 0; chunk < 6; chunk++) {
        if (p.buffered() < 480) {
            p.push(dc.data(), 480);
        }
        p.pull(out, 480);
    }
    CHECK(p.gain_q15() == 32768 && out[479] == 10000);

    // музыка опустела посреди игры: тишина и ожидание (0,15 с), счётчик недобора
    while (p.buffered() > 0) {
        p.pull(out, 480);
    }
    const uint32_t under = p.underruns();
    CHECK(p.pull(out, 480) == 0);
    CHECK(p.push(dc.data(), 1200) == 1200);   // 0,05 с: меньше порога повторной буферизации
    CHECK(p.pull(out, 480) == 0);
    CHECK(p.push(dc.data(), 2400) == 2400);   // теперь 0,15 с
    CHECK(p.pull(out, 480) == 480);
    CHECK(p.underruns() >= under);
    CHECK(p.starved_chunks() >= 1);

    // стоп: буфер выброшен, режим выключен
    p.stop();
    CHECK(!p.active() && p.buffered() == 0 && p.push(dc.data(), 100) == 0);
    while (p.pull(out, 480) > 0) {   // хвост затухания (если звук играл) доигрывается, потом тишина
    }
    CHECK(p.pull(out, 480) == 0 && !p.tail_pending());
    // новый запуск чистит буфер и ставит громкость
    CHECK(p.start(40));
    CHECK(p.volume() == 40 && p.buffered() == 0 && p.starts() == 2);
    CHECK(p.push(dc.data(), 2400) == 2400);
    CHECK(p.start(70));
    CHECK(p.volume() == 70 && p.buffered() == 0 && p.starts() == 3);   // music_start выбрасывает старый буфер
    // громкость вне предела зажимается
    p.set_volume(250);
    CHECK(p.volume() == 100);
    p.set_volume(-3);
    CHECK(p.volume() == 0);
    p.stop();
}

// Стоп посреди звука не щёлкает: режим выключен сразу, а затухающий хвост (~10 мс) доигрывается
static void test_music_player_stop_tail()
{
    using namespace music_logic;
    std::vector<int16_t> store(kCapacitySamples);
    Player p;
    p.init(store.data(), store.size());
    int16_t out[480];
    const std::vector<int16_t> dc = const_audio(9600, 10000);
    CHECK(p.start(100));
    p.push(dc.data(), dc.size());
    CHECK(p.pull(out, 480) == 480 && out[479] == 10000);
    p.stop();
    CHECK(!p.active() && !p.playing() && p.buffered() == 0 && p.tail_pending() && p.wants_stream());
    const size_t n = p.pull(out, 480);
    CHECK(n > 200 && n <= 256);
    CHECK(out[0] > 9800 && out[0] <= 10000);               // продолжает с того уровня, на котором звук оборвался
    bool monotone = true;
    for (size_t i = 1; i < n; i++) {
        if (out[i] > out[i - 1]) {
            monotone = false;
        }
        if (out[i - 1] - out[i] > 120) {                    // не быстрее 10 мс на всю шкалу
            monotone = false;
        }
    }
    CHECK(monotone && out[n - 1] < 100);
    CHECK(!p.tail_pending() && !p.wants_stream() && p.pull(out, 480) == 0);
    // стоп на паузе или в тишине хвоста не даёт
    CHECK(p.start(100));
    p.push(dc.data(), dc.size());
    p.pull(out, 480);
    p.pause(true);
    while (p.pull(out, 480) > 0) {
    }
    p.stop();
    CHECK(!p.tail_pending() && p.pull(out, 480) == 0);
    // повторный stop (сервер подтвердил тап) хвост не убивает
    CHECK(p.start(100));
    p.push(dc.data(), dc.size());
    p.pull(out, 480);
    p.stop();
    p.stop();
    CHECK(p.tail_pending());
    p.pull(out, 480);
    // новый запуск посреди звука: старое затухает, новое нарастает с нуля, старый буфер выброшен
    CHECK(p.start(100));
    p.push(dc.data(), dc.size());
    p.pull(out, 480);
    CHECK(p.start(40));
    CHECK(p.tail_pending() && p.buffered() == 0 && p.volume() == 40);
    const size_t m = p.pull(out, 480);
    CHECK(m > 200 && out[0] > 9800);
    p.stop();
}

static void test_music_player_end_and_done()
{
    using namespace music_logic;
    std::vector<int16_t> store(kCapacitySamples);
    Player p;
    p.init(store.data(), store.size());
    int16_t out[480];
    const std::vector<int16_t> dc = const_audio(24000, 8000);
    // music_end: доиграть остаток, выйти из режима, сказать «готово» ровно один раз
    CHECK(p.start(100));
    CHECK(p.push(dc.data(), 24000) == 24000);   // 1 с
    p.end();
    CHECK(p.ending() && p.active());
    CHECK(p.push(dc.data(), 100) == 0);         // после music_end кадров больше нет
    size_t total = 0;
    int guard = 0;
    while (p.active() && guard++ < 200) {
        total += p.pull(out, 480);
        if (p.take_done()) {
            break;
        }
    }
    CHECK(total == 24000);
    CHECK(!p.active() && p.state() == Player::State::Off);
    CHECK(!p.take_done());                      // второй раз нет
    // music_end при пустом буфере: готово сразу
    CHECK(p.start(40));
    p.end();
    CHECK(p.take_done() && !p.active());
    // music_end на паузе: режим держится (на паузе ничего не играет), «готово» после продолжения и доигрывания
    CHECK(p.start(100));
    p.push(dc.data(), 24000);
    p.pull(out, 480);
    p.pause(true);
    while (p.pull(out, 480) > 0) {
    }
    p.end();
    CHECK(p.active() && p.paused() && !p.take_done());
    CHECK(p.resume());
    total = 0;
    guard = 0;
    bool done = false;
    while (guard++ < 400) {
        total += p.pull(out, 480);
        if (p.take_done()) {
            done = true;
            break;
        }
    }
    CHECK(done && !p.active() && total > 0);
    // переполнение: сервер ушёл вперёд дальше 4 с
    CHECK(p.start(40));
    const std::vector<int16_t> big = const_audio(kCapacitySamples + 1000, 1);
    CHECK(p.push(big.data(), big.size()) == kCapacitySamples);
    CHECK(p.overflowed() == 1000 && p.capacity() == kCapacitySamples);
    p.stop();
}

static void test_music_player_pause_without_audio()
{
    using namespace music_logic;
    std::vector<int16_t> store(kCapacitySamples);
    Player p;
    p.init(store.data(), store.size());
    int16_t out[480];
    // пауза, пока ещё не играли (буфер копится): сразу Paused без щелчка
    CHECK(p.start(40));
    p.pause(false);
    CHECK(p.pull(out, 480) == 0);
    CHECK(p.state() == Player::State::Paused && !p.paused_by_talk());
    // явная пауза сервера поверх паузы платы снимает признак «ждём сервера»
    p.pause(true);
    CHECK(p.paused_by_talk());
    p.pause(false);
    CHECK(!p.paused_by_talk());
    // пауза поставлена в затухании, продолжение прямо в нём: без провала в ноль
    const std::vector<int16_t> dc = const_audio(9600, 10000);
    p.resume();
    p.push(dc.data(), 9600);
    CHECK(p.pull(out, 480) == 480);
    p.pause(true);
    CHECK(p.state() == Player::State::Pausing);
    CHECK(p.resume());
    CHECK(p.state() == Player::State::Playing);
    p.stop();
}

static void test_music_test_signal()
{
    using namespace music_logic;
    std::vector<int16_t> a(48000), b(48000);
    uint64_t pa = 0;
    generate_test_music(a.data(), a.size(), &pa);
    CHECK(pa == 48000);
    // кусками разной длины получается то же самое
    uint64_t pb = 0;
    size_t pos = 0;
    const size_t sizes[] = {1, 7, 480, 1000, 333, 4800};
    int si = 0;
    while (pos < b.size()) {
        size_t n = sizes[si++ % 6];
        if (pos + n > b.size()) {
            n = b.size() - pos;
        }
        generate_test_music(b.data() + pos, n, &pb);
        pos += n;
    }
    CHECK(pb == 48000 && a == b);
    const float peak = dsp::peak_dbfs_i16(a.data(), a.size());
    const float rms = dsp::rms_dbfs_i16(a.data(), a.size());
    CHECK(peak < -1.0f && peak > -8.0f);                   // около -3 dBFS, без среза
    CHECK(rms > -30.0f && rms < -10.0f);                   // не тишина и не шум
    // ритм: в начале каждой доли (бочка) энергия заметно выше, чем в середине доли
    double e_kick = 0.0, e_mid = 0.0;
    for (size_t i = 0; i < 48000; i++) {
        const size_t in_beat = i % 12000;
        const double v = a[i];
        if (in_beat < 1500) {
            e_kick += v * v;
        } else if (in_beat >= 7000 && in_beat < 8500) {
            e_mid += v * v;
        }
    }
    CHECK(e_kick > 1.3 * e_mid);
}

// ---------------------------------------------------------------- разговор: BOOT посреди музыки

static void test_dialog_music()
{
    using dialog::Dialog;
    using dialog::Phase;
    // короткое нажатие BOOT в покое при музыке = стоп: сразу выбросить буфер, послать abort
    {
        Dialog d;
        d.on_link(true, 0);
        d.set_music(true);
        dialog::Out o = d.on_press(100);
        CHECK(!o.music_stop && o.capture_begin);
        o = d.on_tick(150, true);
        CHECK(!o.listen_start && !o.music_stop);
        o = d.on_release(180);   // 80 мс: тап
        CHECK(o.music_stop && o.abort && o.capture_cancel && !o.listen_start && !o.stop_playback);
        CHECK(d.phase() == Phase::Idle && d.music_stops() == 1 && d.aborts() == 0);   // aborts считает отмену ответа, а ответа не было
        proto::OutMsg msgs[3];
        CHECK(dialog::out_messages(o, msgs) == 1 && msgs[0] == proto::OutMsg::Abort);
        // подтверждение сервера (state idle) в покое безвредно: музыки нет, ответа нет, лицо не меняется
        CHECK(!d.on_server_state(proto::ServerState::Idle, nullptr, 250).any());
        CHECK(d.phase() == Phase::Idle && d.accept_text(300));   // «устаревших» сообщений нет: следующий ответ принимается
    }
    // то же нажатие без музыки не шлёт abort (как раньше)
    {
        Dialog d;
        d.on_link(true, 0);
        d.on_press(100);
        const dialog::Out o = d.on_release(180);
        CHECK(!o.music_stop && !o.abort);
    }
    // музыка на паузе по ходу (режим включён) тоже стоп: режим тот же
    {
        Dialog d;
        d.on_link(true, 0);
        d.set_music(true);
        d.on_press(10);
        const dialog::Out o = d.on_release(60);
        CHECK(o.music_stop && o.abort);
    }
    // удержание (запись) при музыке: listen_start, без abort и без стопа музыки; музыку на паузу ставит session по listen_start
    {
        Dialog d;
        d.on_link(true, 0);
        d.set_music(true);
        d.on_press(100);
        const dialog::Out o = d.on_tick(100 + dialog::kHoldMs + 5, true);
        CHECK(o.listen_start && !o.listen_wake && !o.abort && !o.music_stop);
        CHECK(d.phase() == Phase::Listening);
        const dialog::Out r = d.on_release(1000);
        CHECK(r.listen_stop && !r.music_stop && !r.abort);
    }
    // слово при музыке: listen_start (wake) без abort
    {
        Dialog d;
        d.on_link(true, 0);
        d.set_music(true);
        const dialog::Out o = d.on_wake_start(100);
        CHECK(o.listen_start && o.listen_wake && !o.abort && !o.music_stop);
        proto::OutMsg msgs[3];
        CHECK(dialog::out_messages(o, msgs) == 1 && msgs[0] == proto::OutMsg::ListenStartWake);
    }
    // нажатие посреди записи по слову при музыке: конец записи, не стоп музыки
    {
        Dialog d;
        d.on_link(true, 0);
        d.set_music(true);
        d.on_wake_start(100);
        const dialog::Out o = d.on_press(500);
        CHECK(o.listen_stop && !o.music_stop && !o.abort);
        const dialog::Out r = d.on_release(600);
        CHECK(!r.music_stop && !r.abort);   // отпускание после такого нажатия ничего не значит
    }
    // пока Jarvis думает или говорит, тап = abort ответа (как раньше); музыку не трогает (её решает сервер)
    {
        Dialog d;
        d.on_link(true, 0);
        d.set_music(true);
        d.on_wake_start(100);
        d.on_wake_end(2000, false);   // Thinking
        CHECK(d.phase() == Phase::Thinking);
        d.on_press(3000);
        const dialog::Out o = d.on_release(3050);
        CHECK(o.abort && o.stop_playback && !o.music_stop);
    }
    // тап в Error: только убирает ошибку
    {
        Dialog d;
        d.on_link(true, 0);
        d.set_music(true);
        d.on_wake_start(100);
        d.on_wake_end(500, false);
        d.on_server_state(proto::ServerState::Error, "boom", 600);
        CHECK(d.phase() == Phase::Error);
        d.on_press(1000);
        const dialog::Out o = d.on_release(1040);
        CHECK(!o.music_stop && !o.abort && d.phase() == Phase::Idle);
    }
    // без связи тап ничего не шлёт
    {
        Dialog d;
        d.set_music(true);
        d.on_press(10);
        const dialog::Out o = d.on_release(60);
        CHECK(!o.music_stop && !o.abort);
    }
}

// ---------------------------------------------------------------- слово при музыке

static wake_logic::Cond armed_cond()
{
    wake_logic::Cond c;
    c.enabled = c.engine_ok = c.mic_ok = c.wifi_up = c.link_ready = c.dialog_idle = true;
    return c;
}

static void test_wake_music()
{
    using namespace wake_logic;
    // окно follow: без музыки открывается (как раньше), в режиме музыки нет
    {
        Machine m;
        Cond c = armed_cond();
        uint32_t t = 0;
        m.on_tick(c, t);
        c.answers_done = 1;   // ответ закончился нормально
        t += 400;
        m.on_tick(c, t);
        t += kResumeMs + 20;
        m.on_tick(c, t);
        t += 20;
        m.on_tick(c, t);
        CHECK(m.follow_open() && m.state() == State::Armed);
    }
    {
        Machine m;
        Cond c = armed_cond();
        c.music_mode = true;
        uint32_t t = 0;
        m.on_tick(c, t);
        c.answers_done = 1;
        for (int i = 0; i < 100; i++) {
            t += 20;
            m.on_tick(c, t);
        }
        CHECK(m.state() == State::Armed && !m.follow_open() && m.stats().follow_opened == 0);   // слово слушается, окна нет
    }
    // музыка включилась, пока окно открыто или ждёт («Включаю…» → music_start): окно закрывается молча
    {
        Machine m;
        Cond c = armed_cond();
        uint32_t t = 0;
        m.on_tick(c, t);
        c.answers_done = 1;
        for (int i = 0; i < 40; i++) {
            t += 20;
            m.on_tick(c, t);
        }
        CHECK(m.follow_open());
        c.music_mode = true;
        t += 20;
        m.on_tick(c, t);
        CHECK(!m.follow_open() && m.stats().follow_timeouts == 0 && m.state() == State::Armed);
        // и после этого слово работает: запись начинается как обычно
        const Out o = m.on_detect(t + 10);
        CHECK(o.start && !o.follow && !o.barge);
    }
    // звук для движка ослаблен, пока музыка слышна (даже при wake barge off), и ещё kAttenTailMs после
    {
        Machine m;
        m.set_barge(false);
        Cond c = armed_cond();
        uint32_t t = 0;
        m.on_tick(c, t);
        CHECK(!m.engine_attenuated());
        c.music_audible = true;
        t += 20;
        m.on_tick(c, t);
        CHECK(m.engine_attenuated());
        c.music_audible = false;
        t += 20;
        m.on_tick(c, t);
        CHECK(m.engine_attenuated());   // хвост
        t += kAttenTailMs + 20;
        m.on_tick(c, t);
        CHECK(!m.engine_attenuated());
    }
    // без музыки ответ ослабляет только при включённом перебивании (поведение v0.4.2 не изменилось)
    {
        Machine m;
        m.set_barge(false);
        Cond c = armed_cond();
        c.dialog_idle = false;
        c.dialog_answering = c.dialog_speaking = true;
        m.on_tick(c, 0);
        CHECK(!m.engine_attenuated());
        m.set_barge(true);
        m.on_tick(c, 20);
        CHECK(m.engine_attenuated());
    }
    // слово при музыке принимается на взводе как обычно (разговор в покое)
    {
        Machine m;
        Cond c = armed_cond();
        c.music_mode = c.music_audible = true;
        uint32_t t = 0;
        for (int i = 0; i < 40; i++) {
            m.on_tick(c, t);
            t += 20;
        }
        CHECK(m.state() == State::Armed);
        const Out o = m.on_detect(t);
        CHECK(o.start && o.beep);
    }
}

// ---------------------------------------------------------------- лицо при музыке

static void test_face_music()
{
    using namespace face_logic;
    In in;
    in.booted = true;
    in.idle_hint = "Скажи «Джарвис»";
    in.music_line = "\xE2\x99\xAA Capsize";
    Out o = decide(in);
    CHECK(o.face == Face::Idle && strcmp(o.hint, "\xE2\x99\xAA Capsize") == 0);   // вместо подсказки про слово
    in.music_line = "\xE2\x8F\xB8 Capsize";
    CHECK(strcmp(decide(in).hint, "\xE2\x8F\xB8 Capsize") == 0);
    // ход разговора принадлежит разговору: лицо как обычно, без строки музыки (ответ Джарвиса идёт субтитрами)
    in.phase = dialog::Phase::Listening;
    o = decide(in);
    CHECK(o.face == Face::Listening && o.hint[0] == '\0');
    in.phase = dialog::Phase::Thinking;
    CHECK(decide(in).face == Face::Thinking && decide(in).hint[0] == '\0');
    in.phase = dialog::Phase::Speaking;
    CHECK(decide(in).face == Face::Speaking && decide(in).hint[0] == '\0');
    in.phase = dialog::Phase::Idle;
    CHECK(strcmp(decide(in).hint, "\xE2\x8F\xB8 Capsize") == 0);   // после хода строка возвращается
    // нет сервера или Wi-Fi: подсказка про это важнее
    in.server_hint = "Нет связи с ноутбуком";
    o = decide(in);
    CHECK(o.face == Face::NoServer && strcmp(o.hint, "Нет связи с ноутбуком") == 0);
    in.server_hint = nullptr;
    // окно follow при музыке не бывает, но если вдруг: строка музыки главнее «Слушаю…»
    in.follow_window = true;
    CHECK(decide(in).face == Face::Idle);
    in.follow_window = false;
    // без музыки всё как раньше
    in.music_line = nullptr;
    CHECK(strcmp(decide(in).hint, "Скажи «Джарвис»") == 0);
    in.music_line = "";
    CHECK(strcmp(decide(in).hint, "Скажи «Джарвис»") == 0);
}

static void test_music_glyphs()
{
    CHECK(text::font_has_codepoint(0x266A));   // ♪
    CHECK(text::font_has_codepoint(0x23F8));   // ⏸
    CHECK(!text::font_has_codepoint(0x266B));  // ♫ в шрифте нет
    char buf[64];
    text::sanitize("\xE2\x99\xAA Capsize", buf, sizeof(buf));
    CHECK(strcmp(buf, "\xE2\x99\xAA Capsize") == 0);
    text::sanitize("\xE2\x8F\xB8 Кино", buf, sizeof(buf));
    CHECK(strcmp(buf, "\xE2\x8F\xB8 Кино") == 0);
    text::sanitize("\xE2\x99\xAB x", buf, sizeof(buf));   // ♫ нет в шрифте: «?»
    CHECK(strcmp(buf, "? x") == 0);
}

// ---------------------------------------------------------------- AEC: ресемплер, кольца, выравнивание

static double goertzel_amp(const std::vector<int16_t> &x, size_t from, size_t to, double fs, double f)
{
    // амплитуда синуса частоты f в x[from..to) (дискретное преобразование Фурье на одной частоте)
    double re = 0.0, im = 0.0;
    const double w = 2.0 * 3.14159265358979323846 * f / fs;
    for (size_t i = from; i < to; i++) {
        re += x[i] * cos(w * static_cast<double>(i - from));
        im -= x[i] * sin(w * static_cast<double>(i - from));
    }
    return 2.0 * sqrt(re * re + im * im) / static_cast<double>(to - from);
}

static std::vector<int16_t> sine24(size_t n, double f, double amp)
{
    std::vector<int16_t> x(n);
    for (size_t i = 0; i < n; i++) {
        x[i] = static_cast<int16_t>(lrint(amp * sin(2.0 * 3.14159265358979323846 * f * static_cast<double>(i) / 24000.0)));
    }
    return x;
}

static std::vector<int16_t> run_resampler(aec_logic::Resampler24to16 &rs, const std::vector<int16_t> &in, size_t chunk)
{
    std::vector<int16_t> out;
    static int16_t tmp[12000];   // выход не короче n * 2 / 3 + 2 для самого длинного куска
    for (size_t pos = 0; pos < in.size(); pos += chunk) {
        const size_t n = std::min(chunk, in.size() - pos);
        const size_t k = rs.process(in.data() + pos, n, tmp, sizeof(tmp) / sizeof(tmp[0]));
        out.insert(out.end(), tmp, tmp + k);
    }
    return out;
}

static void test_aec_resampler()
{
    using aec_logic::Resampler24to16;
    // 1 кГц: амплитуда сохраняется, число отсчётов 2/3
    {
        Resampler24to16 rs;
        const std::vector<int16_t> in = sine24(24000, 1000.0, 10000.0);
        const std::vector<int16_t> out = run_resampler(rs, in, 480);
        CHECK(out.size() >= 15980 && out.size() <= 15990);   // 2/3 входа минус задержка фильтра (24 отсчёта входа)
        CHECK_NEAR(goertzel_amp(out, 1000, 14000, 16000.0, 1000.0), 10000.0, 150.0);
    }
    // 5 кГц проходит (в пределах 1 дБ), 10,5 кГц (отразилась бы на 5,5 кГц) подавлена больше чем на 30 дБ
    {
        Resampler24to16 rs;
        const std::vector<int16_t> in = sine24(24000, 5000.0, 10000.0);
        const std::vector<int16_t> out = run_resampler(rs, in, 480);
        const double a = goertzel_amp(out, 1000, 14000, 16000.0, 5000.0);
        CHECK(a > 10000.0 * 0.89 && a < 10000.0 * 1.12);
    }
    {
        Resampler24to16 rs;
        const std::vector<int16_t> in = sine24(24000, 10500.0, 10000.0);
        const std::vector<int16_t> out = run_resampler(rs, in, 480);
        const double a = goertzel_amp(out, 1000, 14000, 16000.0, 5500.0);
        CHECK(a < 10000.0 * 0.032);   // > 30 дБ вниз
    }
    // постоянная составляющая: усиление 1
    {
        Resampler24to16 rs;
        const std::vector<int16_t> in(4800, 12345);
        const std::vector<int16_t> out = run_resampler(rs, in, 480);
        CHECK(out[out.size() / 2] >= 12340 && out[out.size() / 2] <= 12350);
    }
    // кусками любой длины результат один и тот же
    {
        const std::vector<int16_t> in = sine24(9600, 1500.0, 9000.0);
        Resampler24to16 a, b, c;
        const std::vector<int16_t> o1 = run_resampler(a, in, 480);
        const std::vector<int16_t> o2 = run_resampler(b, in, 1);
        const std::vector<int16_t> o3 = run_resampler(c, in, 37);
        CHECK(o1 == o2 && o1 == o3 && o1.size() > 6000);
        // кусок длиннее рабочего буфера режется внутри
        Resampler24to16 d;
        const std::vector<int16_t> o4 = run_resampler(d, in, 9600);
        CHECK(o1 == o4);
    }
    // reset: начало потока с нуля (нет хвоста прошлого звука)
    {
        Resampler24to16 rs;
        run_resampler(rs, sine24(2400, 1000.0, 20000.0), 480);
        rs.reset();
        CHECK(rs.consumed() == 0);
        const std::vector<int16_t> out = run_resampler(rs, std::vector<int16_t>(480, 0), 480);
        bool zero = true;
        for (int16_t v : out) {
            zero = zero && v == 0;
        }
        CHECK(zero);
    }
    // насыщение не заворачивается: полная шкала не даёт знакопеременного мусора
    {
        Resampler24to16 rs;
        std::vector<int16_t> in(2400, 32767);
        const std::vector<int16_t> out = run_resampler(rs, in, 480);
        CHECK(out[out.size() / 2] >= 32000);
    }
}

static void test_aec_history()
{
    using aec_logic::History16;
    std::vector<int16_t> mem(16);
    History16 h;
    h.init(mem.data(), 16);
    CHECK(h.end() == 0 && h.capacity() == 16);
    int16_t src[10];
    for (int i = 0; i < 10; i++) {
        src[i] = static_cast<int16_t>(100 + i);
    }
    CHECK(h.write(src, 10) == 10);
    int16_t dst[16];
    CHECK(h.read(0, dst, 10) == 10 && dst[0] == 100 && dst[9] == 109);
    // ещё не записанное и ушедшее до начала даёт нули
    CHECK(h.read(5, dst, 10) == 5 && dst[0] == 105 && dst[4] == 109 && dst[5] == 0 && dst[9] == 0);
    CHECK(h.read(-3, dst, 6) == 3 && dst[0] == 0 && dst[2] == 0 && dst[3] == 100);
    // переход через конец кольца: второй десяток ложится на 10..19, самое старое (номера 0..3) затёрто
    CHECK(h.write(src, 10) == 20);
    CHECK(h.read(4, dst, 16) == 16 && dst[0] == 104 && dst[5] == 109 && dst[6] == 100 && dst[15] == 109);
    CHECK(h.read(2, dst, 4) == 2 && dst[0] == 0 && dst[1] == 0 && dst[2] == 104 && dst[3] == 105);
    // запись длиннее кольца оставляет последний кусок
    std::vector<int16_t> big(40);
    for (int i = 0; i < 40; i++) {
        big[i] = static_cast<int16_t>(i);
    }
    const int64_t e = h.write(big.data(), 40);
    CHECK(e == 36 && h.read(e - 1, dst, 1) == 1 && dst[0] == 39 && h.read(e - 16, dst, 1) == 1 && dst[0] == 24);
    CHECK(h.read(0, dst, 0) == 0);
}

static void test_aec_window_min_and_hangover()
{
    using namespace aec_logic;
    WindowMin w;
    w.set_window(4);
    CHECK(!w.valid());
    w.add(50);
    w.add(30);
    w.add(70);
    CHECK(w.valid() && w.count() == 3 && w.min() == 30);
    w.add(60);   // окно из четырёх последних: 50 30 70 60
    CHECK(w.min() == 30);
    w.add(80);   // 30 70 60 80
    CHECK(w.min() == 30);
    w.add(90);   // 70 60 80 90: 30 вышло из окна
    CHECK(w.min() == 60);
    w.add(100);  // 60 80 90 100
    CHECK(w.min() == 60);
    w.add(110);  // 80 90 100 110
    CHECK(w.min() == 80);
    w.clear();
    CHECK(!w.valid());

    // хвост 0,6 с после конца звука
    Hangover h;
    CHECK(!h.active());
    h.update(true, 1000);
    CHECK(h.active());
    h.update(false, 1300);
    CHECK(h.active());
    h.update(false, 1000 + kHangoverMs - 1);
    CHECK(h.active());
    h.update(false, 1000 + kHangoverMs);
    CHECK(!h.active());
    h.update(true, 5000);
    h.update(false, 5000 + 100);
    h.update(true, 5000 + 200);   // продолжило играть: отсчёт хвоста заново
    h.update(false, 5000 + 300);
    CHECK(h.active());
    // через переполнение 32 бит мс
    Hangover o;
    o.update(true, 0xFFFFFF00u);
    o.update(false, 0xFFFFFF00u + 100);
    CHECK(o.active());
    o.update(false, 0xFFFFFF00u + kHangoverMs + 1);
    CHECK(!o.active());
    h.reset();
    CHECK(!h.active());
}

// Модель времени для проверки выравнивания. Одна временная ось в мкс.
//   - динамик: поток 24 кГц, куски по 20 мс. Запись куска k возвращается за 60 мс до того, как его последний отсчёт прозвучит (глубина DMA 6 x 10 мс);
//     первые куски потока возвращаются без ожидания (пустая очередь), остальные «в такт» с дрожанием 0..800 мкс;
//   - эхо приходит в микрофон через prop_us после того, как прозвучало;
//   - блок микрофона (1600 отсчётов) приходит через lat_us после своего конца, с дрожанием и редким опозданием.
struct AlignSim {
    int64_t emit0_us = 1000000;      // когда прозвучал нулевой отсчёт потока
    int64_t mic0_us = 1006000;       // когда был захвачен нулевой отсчёт микрофона (емит + 6000 мкс на распространение и микрофон)
    int64_t lat_us = 1500;           // задержка прихода блока микрофона
    uint32_t rng = 12345;
    int64_t jitter(int64_t max_us)
    {
        rng = rng * 1664525u + 1013904223u;
        return static_cast<int64_t>((rng >> 8) % static_cast<uint32_t>(max_us + 1));
    }
};

static void test_aec_aligner_simulation()
{
    using namespace aec_logic;
    AlignSim s;
    Aligner al;
    CHECK(!al.ready());
    // идёт микрофон 3 с (блоки по 100 мс)
    int64_t mic_end = 0;
    auto mic_blocks_until = [&](int64_t t_us) {
        while (true) {
            const int64_t end_idx = mic_end + 1600;
            const int64_t t_end = s.mic0_us + samples_to_us(end_idx);   // момент конца блока
            const int64_t arr = t_end + s.lat_us + s.jitter(3000) + (s.jitter(40) == 0 ? 60000 : 0);   // редкое опоздание на 60 мс
            if (t_end > t_us) {
                break;
            }
            mic_end = end_idx;
            al.mic_block(end_idx, arr);
        }
    };
    mic_blocks_until(s.emit0_us + 1000000);
    CHECK(!al.ready());   // динамик ещё не играл

    // поток: первые три куска без ожидания (раньше срока на 40..60 мс), дальше «в такт»
    int64_t ref_end = 0;
    int64_t expect_diff_us = 0;
    for (int k = 0; k < 60; k++) {
        const int64_t chunk_end_emit = s.emit0_us + static_cast<int64_t>(k + 1) * 20000;   // прозвучит последний отсчёт куска
        ref_end += 320;
        const bool burst = k < 3;
        const int64_t t_ret = burst ? chunk_end_emit - 60000 - 40000 + s.jitter(3000)       // без ожидания: заметно раньше такта
                                    : chunk_end_emit - 60000 + s.jitter(800);
        al.ref_chunk(ref_end, t_ret, !burst, k == 0);
        mic_blocks_until(t_ret + 200000);
    }
    CHECK(al.ready());
    // Разница якорей должна равняться A_mic - A_spk модели: A_mic = mic0 + lat (минимум по блокам), A_spk = emit0 - 60000 (минимум по тактовым возвратам)
    expect_diff_us = (s.mic0_us + s.lat_us) - (s.emit0_us - 60000);
    const int64_t pos0 = al.ref_pos_for(10000, 60000);
    const int64_t diff = al.anchor_diff_us();
    CHECK(diff > expect_diff_us - 1200 && diff < expect_diff_us + 1500);   // ошибка порядка 1 мс (дрожание), а не 40..60 мс «пустой очереди» или 60 мс опоздания
    CHECK(pos0 == 10000 + us_to_samples(diff - 60000));
    CHECK(al.streams() == 1);

    // дрожание меньше порога не двигает принятое выравнивание (чтобы опора не прыгала на кадрах)
    const int64_t applied = al.anchor_diff_us();
    for (int k = 60; k < 100; k++) {
        ref_end += 320;
        const int64_t chunk_end_emit = s.emit0_us + static_cast<int64_t>(k + 1) * 20000;
        al.ref_chunk(ref_end, chunk_end_emit - 60000 + s.jitter(1000), true, false);
        al.ref_pos_for(20000, 60000);
    }
    CHECK(al.anchor_diff_us() >= applied - 1500 && al.anchor_diff_us() <= applied + 1500);

    // новый поток через паузу: сдвиг другой (динамик стартовал позже), оценка начинается заново и обновляется скачком
    const int64_t gap_us = 2000000;
    const int64_t emit1 = s.emit0_us + 100 * 20000 + gap_us;
    for (int k = 0; k < 40; k++) {
        const int64_t chunk_end_emit = emit1 + static_cast<int64_t>(k + 1) * 20000;
        ref_end += 320;
        const bool burst = k < 3;
        const int64_t t_ret = burst ? chunk_end_emit - 60000 - 45000 : chunk_end_emit - 60000 + s.jitter(800);
        al.ref_chunk(ref_end, t_ret, !burst, k == 0);
        mic_blocks_until(t_ret + 200000);
    }
    al.ref_pos_for(30000, 60000);
    // ref_end продолжает счёт (320 на кусок), а время ушло вперёд на gap_us + 100 кусков: якорь динамика сдвинулся на (время - отсчёты * 62,5)
    const int64_t new_expect = (s.mic0_us + s.lat_us) - ((emit1 - 60000) - samples_to_us(static_cast<int64_t>(100) * 320));
    const int64_t d2 = al.anchor_diff_us();
    CHECK(d2 > new_expect - 1200 && d2 < new_expect + 1500);
    CHECK(al.streams() == 2 && al.jumps() >= 1);
    // сброс
    al.reset();
    CHECK(!al.ready() && al.jumps() == 0 && al.streams() == 0);
}

// Сквозная проверка: звук идёт через ресемплер, кольцо и выравнивание, а эхо в микрофоне задержано по физической модели; корреляция опоры и микрофона
// даёт ту задержку, которую предсказывает модель
static void test_aec_end_to_end_alignment()
{
    using namespace aec_logic;
    SyntheticEcho src(0, 0.0f);   // только источник опоры (x16)
    constexpr int kSeconds = 6;
    const size_t n16 = 16000 * kSeconds;
    std::vector<int16_t> x16(n16), unused(n16);
    src.generate(x16.data(), unused.data(), n16, 0.0f);

    // Динамик получает x16, поднятый до 24 кГц линейной интерполяцией (полоса 3 кГц: ошибка мала)
    const size_t n24 = n16 * 3 / 2;
    std::vector<int16_t> x24(n24);
    for (size_t i = 0; i < n24; i++) {
        const double p = static_cast<double>(i) * 2.0 / 3.0;
        const size_t j = static_cast<size_t>(p);
        const double fr = p - static_cast<double>(j);
        const double a = x16[j], b = j + 1 < n16 ? x16[j + 1] : x16[j];
        x24[i] = static_cast<int16_t>(lrint(a + (b - a) * fr));
    }
    AlignSim s;
    s.emit0_us = 2000000;
    const int64_t prop_us = 1000, mic_lat_us = 5000;
    // эхо: отсчёт x16[n] звучит в E0 + 62,5 n, достигает микрофона на prop_us позже, а микрофон видит его ещё на mic_lat_us позже
    // => mic[i] = 0,3 * x16[i + K], где tc(i) = C0 + 62,5 i, C0 = E0 + prop + mic_lat + K * 62,5 (K = 0 выбираем так)
    s.mic0_us = s.emit0_us + prop_us + mic_lat_us;
    const int64_t K = 0;
    std::vector<int16_t> mic(n16, 0);
    for (size_t i = 0; i < n16; i++) {
        const int64_t n = static_cast<int64_t>(i) + K;
        mic[i] = (n >= 0 && n < static_cast<int64_t>(n16)) ? static_cast<int16_t>(x16[static_cast<size_t>(n)] * 3 / 10) : 0;
    }
    Resampler24to16 rs;
    History16 ref;
    std::vector<int16_t> ref_mem(131072);   // на плате кольцо опоры 1 с; в тесте читаем после всего прогона, поэтому с запасом
    ref.init(ref_mem.data(), 131072);
    Aligner al;
    // Крутим время: куски динамика по 20 мс и блоки микрофона по 100 мс в порядке их времён
    int64_t mic_end = 0;
    size_t pos24 = 0;
    int k = 0;
    while (pos24 + 480 <= n24 && k < 280) {
        const int64_t chunk_end_emit = s.emit0_us + static_cast<int64_t>(k + 1) * 20000;
        int16_t tmp[400];
        const size_t got = rs.process(x24.data() + pos24, 480, tmp, 400);
        const int64_t end = ref.write(tmp, got);
        const bool burst = k < 3;
        const int64_t t_ret = burst ? chunk_end_emit - 60000 - 40000 : chunk_end_emit - 60000 + s.jitter(600);
        al.ref_chunk(end, t_ret, !burst, k == 0);
        pos24 += 480;
        k++;
        // блоки микрофона, которые к этому времени уже закончились (по mic0 + конец блока + задержка прихода)
        while (true) {
            const int64_t end_idx = mic_end + 1600;
            const int64_t arr = s.mic0_us + samples_to_us(end_idx) + s.lat_us + s.jitter(2500);
            if (arr > t_ret + 60000 + 5000 || static_cast<size_t>(end_idx) > n16) {
                break;
            }
            mic_end = end_idx;
            al.mic_block(end_idx, arr);
        }
    }
    CHECK(al.ready());
    // Берём микрофон на отрезке 2..4 с и находим опору, выровненную с delay = 60 мс
    const int delay_ms = 60;
    const int64_t i0 = 16000 * 2;
    const size_t len = 16000 * 2;
    std::vector<int16_t> ref_al(len);
    const int64_t rpos = al.ref_pos_for(i0, static_cast<int64_t>(delay_ms) * 1000);
    ref.read(rpos, ref_al.data(), len);
    std::vector<int16_t> scratch(xcorr_scratch_samples(len));
    const XcorrResult r = xcorr_best_lag(ref_al.data(), mic.data() + i0, len, -400, 1600, scratch.data());
    CHECK(r.valid && r.peak > 0.5f);
    // Ожидание по модели. Якорь микрофона: mic0 + lat (минимум по блокам). Якорь динамика: ресемплер выдаёт содержимое без сдвига, но на kHalf отсчётов входа (1 мс)
    // позже, поэтому счёт отсчётов опоры отстаёт от времени записи на 1 мс: A_spk = emit0 - 60 мс + 1 мс. Разность якорей = (mic0 - emit0) + lat + 59 мс =
    // 6 + 1,5 + 59 = 66,5 мс. С delay 60 мс опора берётся с отсчёта, на 6,5 мс (104 отсчёта) новее; эхо в модели mic[i] = x16[i], а содержимое
    // опоры не сдвинуто, значит эхо отстаёт от выровненной опоры ровно на эти 104 отсчёта (с точностью до дрожания, порядка 1-2 отсчётов).
    CHECK(r.lag >= 100 && r.lag <= 108);
    // и сдвиг delay на 10 мс двигает найденный лаг ровно на 160 отсчётов
    const int64_t rpos2 = al.ref_pos_for(i0, static_cast<int64_t>(delay_ms + 10) * 1000);
    CHECK(rpos2 == rpos - 160);
    std::vector<int16_t> ref_al2(len);
    ref.read(rpos2, ref_al2.data(), len);
    const XcorrResult r2 = xcorr_best_lag(ref_al2.data(), mic.data() + i0, len, -400, 1600, scratch.data());
    CHECK(r2.valid && r2.lag == r.lag - 160);   // опора старше на 160 отсчётов: эхо теперь «впереди» неё (поэтому delay ставят чуть меньше настоящего)
}

static void test_aec_erle_meter()
{
    using aec_logic::ErleMeter;
    ErleMeter m;
    std::vector<int16_t> mic(512), out(512), ref(512), quiet(512, 3);
    for (int i = 0; i < 512; i++) {
        mic[i] = static_cast<int16_t>(8000 * sin(i * 0.3));
        out[i] = static_cast<int16_t>(mic[i] / 10);   // -20 дБ
        ref[i] = static_cast<int16_t>(9000 * sin(i * 0.31));
    }
    m.add(mic.data(), out.data(), quiet.data(), 512);   // опора тихая: кадр пропущен
    CHECK(m.frames() == 0 && m.skipped() == 1 && m.erle_db() == 0.0f);
    m.add(mic.data(), out.data(), ref.data(), 512);
    m.add(mic.data(), out.data(), ref.data(), 512);
    CHECK(m.frames() == 2 && m.skipped() == 1);
    CHECK_NEAR(m.erle_db(), 20.0, 0.15);
    CHECK_NEAR(m.mic_rms_dbfs(), 20.0 * log10(8000.0 / 1.41421356 / 32768.0), 0.2);
    CHECK_NEAR(m.out_rms_dbfs(), m.mic_rms_dbfs() - 20.0, 0.2);
    CHECK(m.ref_rms_dbfs() > m.mic_rms_dbfs());
    m.clear();
    CHECK(m.frames() == 0 && m.erle_db() == 0.0f && m.mic_rms_dbfs() == -120.0f);
}

static void test_aec_xcorr_and_synthetic()
{
    using namespace aec_logic;
    // эхо задержано на 200 отсчётов (12,5 мс), усиление 0,25: корреляция находит задержку
    SyntheticEcho echo(200, 0.25f);
    const size_t n = 24000;
    std::vector<int16_t> ref(n), mic(n), scratch(xcorr_scratch_samples(n));
    echo.generate(ref.data(), mic.data(), n, 0.0f);
    XcorrResult r = xcorr_best_lag(ref.data(), mic.data(), n, -640, 4000, scratch.data());
    CHECK(r.valid && r.lag >= 199 && r.lag <= 201 && r.peak > 0.4f && r.ratio > 4.0f);
    // задержка 1000 отсчётов (62,5 мс), с речью поверх
    SyntheticEcho echo2(1000, 0.15f);
    echo2.generate(ref.data(), mic.data(), n, 1500.0f);
    r = xcorr_best_lag(ref.data(), mic.data(), n, -640, 4000, scratch.data());
    CHECK(r.valid && r.lag == 1000);
    // отрицательный лаг (микрофон раньше опоры) тоже находится
    std::vector<int16_t> ref3(n), mic3(n);
    for (size_t i = 0; i < n; i++) {
        ref3[i] = ref[i];
        mic3[i] = i + 300 < n ? ref[i + 300] / 4 : 0;
    }
    r = xcorr_best_lag(ref3.data(), mic3.data(), n, -640, 4000, scratch.data());
    CHECK(r.valid && r.lag == -300);
    // не связанные сигналы: корреляции нет
    SyntheticEcho other(0, 0.0f);
    std::vector<int16_t> r2(n), m2(n);
    other.generate(r2.data(), m2.data(), n, 3000.0f);   // mic = шум + речь, к опоре отношения не имеет
    r = xcorr_best_lag(r2.data(), m2.data(), n, -640, 4000, scratch.data());
    CHECK(!r.valid);
    // слишком короткие данные и мусорные пределы
    r = xcorr_best_lag(ref.data(), mic.data(), 100, 0, 100, scratch.data());
    CHECK(!r.valid);
    r = xcorr_best_lag(ref.data(), mic.data(), n, 50, 10, scratch.data());
    CHECK(!r.valid);
    // шум для измерения: пик ровно -12 dBFS, плавные края, нет постоянной составляющей
    std::vector<int16_t> noise(43200);
    generate_cal_noise(noise.data(), noise.size(), 24000, -12.0f);
    CHECK_NEAR(dsp::peak_dbfs_i16(noise.data(), noise.size()), -12.0, 0.15);
    CHECK(noise[0] == 0 && std::abs(noise[5]) < 300);   // плавное нарастание (первые 20 мс)
    double sum = 0.0;
    for (int16_t v : noise) {
        sum += v;
    }
    CHECK(std::fabs(sum / static_cast<double>(noise.size())) < 40.0);
    CHECK(dsp::rms_dbfs_i16(noise.data() + 2000, 30000) > -40.0f);
    // синтетика детерминирована
    SyntheticEcho a(200, 0.25f), b(200, 0.25f);
    std::vector<int16_t> ra(2000), ma(2000), rb(2000), mb(2000);
    a.generate(ra.data(), ma.data(), 2000, 100.0f);
    b.generate(rb.data(), mb.data(), 2000, 100.0f);
    CHECK(ra == rb && ma == mb);
    b.reset();
    b.generate(rb.data(), mb.data(), 2000, 100.0f);
    CHECK(ra == rb && ma == mb);
}

static void test_aec_config_logic()
{
    using namespace aec_logic;
    Mode m;
    CHECK(parse_mode("fd_low", &m) && m == Mode::FdLow);
    CHECK(parse_mode("SR_LOW", &m) && m == Mode::SrLow);
    CHECK(parse_mode("Sr_High", &m) && m == Mode::SrHigh);
    CHECK(parse_mode("fd_high", &m) && m == Mode::FdHigh);
    CHECK(!parse_mode("voip", &m) && !parse_mode("", &m) && !parse_mode(nullptr, &m));
    CHECK(strcmp(mode_name(Mode::FdLow), "fd_low") == 0 && strcmp(mode_name(Mode::SrHigh), "sr_high") == 0);
    // номера aec_mode_t из esp_aec.h: SR_LOW 0, SR_HIGH 1, FD_LOW 5, FD_HIGH 6
    CHECK(mode_to_esp(Mode::SrLow) == 0 && mode_to_esp(Mode::SrHigh) == 1 && mode_to_esp(Mode::FdLow) == 5 && mode_to_esp(Mode::FdHigh) == 6);
    CHECK(mode_is_fd(Mode::FdLow) && !mode_is_fd(Mode::SrHigh));
    CHECK(delay_valid(0) && delay_valid(250) && !delay_valid(-1) && !delay_valid(251));
    CHECK(taps_valid(1) && taps_valid(8) && !taps_valid(0) && !taps_valid(9));
    CHECK(nlp_valid(0) && nlp_valid(2) && !nlp_valid(3) && !nlp_valid(-1));
    // из NVS: мусор -> по умолчанию
    CHECK(delay_from_saved(false, 100) == kDelayDefaultMs && delay_from_saved(true, 100) == 100 && delay_from_saved(true, 9999) == kDelayDefaultMs);
    CHECK(taps_from_saved(true, 6) == 6 && taps_from_saved(true, 0) == kTapsDefault && taps_from_saved(false, 6) == kTapsDefault);
    CHECK(mode_from_saved(true, 3) == Mode::FdHigh && mode_from_saved(true, 7) == Mode::FdLow && mode_from_saved(false, 0) == Mode::FdLow);
    CHECK(nlp_from_saved(true, 2) == 2 && nlp_from_saved(true, 5) == 0);
    Config c;
    CHECK(c.enabled && c.voice && c.delay_ms == 60 && c.taps == 4 && c.mode == Mode::FdLow && c.nlp == 0);
    CHECK(samples_to_us(16) == 1000 && samples_to_us(1) == 62 && us_to_samples(1000) == 16 && us_to_samples(-1000) == -16 && us_to_samples(0) == 0);
}

// ---------------------------------------------------------------- команды aec и music

static void test_commands_v060()
{
    using cmd::AecOp;
    using cmd::Kind;
    using cmd::MusicOp;
    cmd::Command c = cmd::parse_command("aec");
    CHECK(c.kind == Kind::Aec && c.aec_op == AecOp::Status);
    c = cmd::parse_command("  AEC   on ");
    CHECK(c.kind == Kind::Aec && c.aec_op == AecOp::On);
    c = cmd::parse_command("aec off");
    CHECK(c.kind == Kind::Aec && c.aec_op == AecOp::Off);
    c = cmd::parse_command("aec voice on");
    CHECK(c.kind == Kind::Aec && c.aec_op == AecOp::VoiceOn);
    c = cmd::parse_command("aec voice OFF");
    CHECK(c.kind == Kind::Aec && c.aec_op == AecOp::VoiceOff);
    c = cmd::parse_command("aec voice maybe");
    CHECK(c.kind == Kind::Error && strcmp(c.err_reason, "usage") == 0);
    c = cmd::parse_command("aec delay 72");
    CHECK(c.kind == Kind::Aec && c.aec_op == AecOp::Delay && c.aec_value == 72);
    c = cmd::parse_command("aec delay 0");
    CHECK(c.kind == Kind::Aec && c.aec_op == AecOp::Delay && c.aec_value == 0);
    c = cmd::parse_command("aec delay 250");
    CHECK(c.kind == Kind::Aec && c.aec_value == 250);
    c = cmd::parse_command("aec delay 251");
    CHECK(c.kind == Kind::Error && strcmp(c.err_reason, "delay_range") == 0);
    c = cmd::parse_command("aec delay -4");
    CHECK(c.kind == Kind::Error && strcmp(c.err_reason, "delay_range") == 0);
    c = cmd::parse_command("aec delay");
    CHECK(c.kind == Kind::Error && strcmp(c.err_reason, "usage") == 0);
    c = cmd::parse_command("aec delay 60ms");
    CHECK(c.kind == Kind::Error && strcmp(c.err_reason, "usage") == 0);
    c = cmd::parse_command("aec taps 8");
    CHECK(c.kind == Kind::Aec && c.aec_op == AecOp::Taps && c.aec_value == 8);
    c = cmd::parse_command("aec taps 0");
    CHECK(c.kind == Kind::Error && strcmp(c.err_reason, "taps_range") == 0);
    c = cmd::parse_command("aec nlp 2");
    CHECK(c.kind == Kind::Aec && c.aec_op == AecOp::Nlp && c.aec_value == 2);
    c = cmd::parse_command("aec nlp 3");
    CHECK(c.kind == Kind::Error && strcmp(c.err_reason, "nlp_range") == 0);
    c = cmd::parse_command("aec mode sr_low");
    CHECK(c.kind == Kind::Aec && c.aec_op == AecOp::Mode && c.aec_value == static_cast<int>(aec_logic::Mode::SrLow));
    c = cmd::parse_command("aec mode FD_HIGH");
    CHECK(c.kind == Kind::Aec && c.aec_op == AecOp::Mode && c.aec_value == static_cast<int>(aec_logic::Mode::FdHigh));
    c = cmd::parse_command("aec mode voip");
    CHECK(c.kind == Kind::Error && strcmp(c.err_reason, "mode_value") == 0);
    c = cmd::parse_command("aec mode");
    CHECK(c.kind == Kind::Error && strcmp(c.err_reason, "usage") == 0);
    c = cmd::parse_command("aec bench");
    CHECK(c.kind == Kind::Aec && c.aec_op == AecOp::Bench);
    c = cmd::parse_command("aec bench now");
    CHECK(c.kind == Kind::Error);
    c = cmd::parse_command("aec cal");
    CHECK(c.kind == Kind::Aec && c.aec_op == AecOp::Cal);
    c = cmd::parse_command("aec frobnicate");
    CHECK(c.kind == Kind::Error && strcmp(c.err_cmd, "aec") == 0 && strcmp(c.err_reason, "usage") == 0);
    c = cmd::parse_command("aec on now");
    CHECK(c.kind == Kind::Error);

    c = cmd::parse_command("music");
    CHECK(c.kind == Kind::Music && c.music_op == MusicOp::Stat);
    c = cmd::parse_command("music stat");
    CHECK(c.kind == Kind::Music && c.music_op == MusicOp::Stat);
    c = cmd::parse_command("music vol 55");
    CHECK(c.kind == Kind::Music && c.music_op == MusicOp::Vol && c.music_volume == 55);
    c = cmd::parse_command("music volume 0");
    CHECK(c.kind == Kind::Music && c.music_op == MusicOp::Vol && c.music_volume == 0);
    c = cmd::parse_command("music vol 101");
    CHECK(c.kind == Kind::Error && strcmp(c.err_reason, "vol_range") == 0);
    c = cmd::parse_command("music vol");
    CHECK(c.kind == Kind::Error && strcmp(c.err_reason, "usage") == 0);
    c = cmd::parse_command("music vol loud");
    CHECK(c.kind == Kind::Error && strcmp(c.err_reason, "usage") == 0);
    c = cmd::parse_command("music test");
    CHECK(c.kind == Kind::Music && c.music_op == MusicOp::Test && c.music_volume == -1);
    c = cmd::parse_command("music test 30");
    CHECK(c.kind == Kind::Music && c.music_op == MusicOp::Test && c.music_volume == 30);
    c = cmd::parse_command("music test 300");
    CHECK(c.kind == Kind::Error && strcmp(c.err_reason, "vol_range") == 0);
    c = cmd::parse_command("music pause");
    CHECK(c.kind == Kind::Music && c.music_op == MusicOp::Pause);
    c = cmd::parse_command("music RESUME");
    CHECK(c.kind == Kind::Music && c.music_op == MusicOp::Resume);
    c = cmd::parse_command("music stop");
    CHECK(c.kind == Kind::Music && c.music_op == MusicOp::Stop);
    c = cmd::parse_command("music stop now");
    CHECK(c.kind == Kind::Error);
    c = cmd::parse_command("music dance");
    CHECK(c.kind == Kind::Error && strcmp(c.err_cmd, "music") == 0);
    // описания ошибок по-русски
    CHECK(strstr(cmd::describe_error("delay_range"), "250") != nullptr);
    CHECK(strstr(cmd::describe_error("mode_value"), "fd_low") != nullptr);
    CHECK(strstr(cmd::describe_error("vol_range"), "100") != nullptr);
    CHECK(strstr(cmd::describe_error("taps_range"), "8") != nullptr);
    CHECK(strstr(cmd::describe_error("nlp_range"), "2") != nullptr);
    // старые команды не пострадали
    c = cmd::parse_command("wake bench");
    CHECK(c.kind == Kind::Wake);
    c = cmd::parse_command("echo");
    CHECK(c.kind == Kind::Echo);
}

// Ход «слово посреди музыки» целиком по логике: слово (listen_start wake) -> ответ -> тап стоп; окно follow не открывается
static void test_music_turn_flow()
{
    using dialog::Dialog;
    Dialog d;
    wake_logic::Machine m;
    d.on_link(true, 0);
    d.set_music(true);
    wake_logic::Cond c;
    c.enabled = c.engine_ok = c.mic_ok = c.wifi_up = c.link_ready = true;
    c.music_mode = c.music_audible = true;
    uint32_t t = 0;
    auto tick = [&](uint32_t dt) {
        t += dt;
        const dialog::Out o = d.on_tick(t, true);
        (void)o;
        const auto v = d.phase();
        c.dialog_idle = v == dialog::Phase::Idle;
        c.dialog_listening = v == dialog::Phase::Listening;
        c.dialog_answering = v == dialog::Phase::Thinking || v == dialog::Phase::Speaking;
        c.dialog_speaking = v == dialog::Phase::Speaking;
        c.answers_done = d.answers_done();
        return m.on_tick(c, t);
    };
    for (int i = 0; i < 30; i++) {
        tick(20);
    }
    CHECK(m.state() == wake_logic::State::Armed);
    // слово
    const wake_logic::Out o = m.on_detect(t);
    CHECK(o.start && !o.barge);
    const dialog::Out ds = d.on_wake_start(t);
    CHECK(ds.listen_start && ds.listen_wake && !ds.abort);
    // фраза, конец, ответ голосом
    for (int i = 0; i < 20; i++) {
        tick(20);
    }
    m.on_speech(t);
    d.on_wake_end(t, false);
    tick(20);
    d.on_server_state(proto::ServerState::Thinking, nullptr, t + 5);
    d.on_audio_start(t + 10);
    d.on_server_state(proto::ServerState::Speaking, nullptr, t + 15);
    d.on_audio_end(t + 20);
    d.on_server_state(proto::ServerState::Idle, nullptr, t + 25);
    for (int i = 0; i < 50; i++) {
        tick(20);
    }
    CHECK(d.phase() == dialog::Phase::Idle && d.answers_done() == 1);
    // ответ закончился нормально, но в режиме музыки окно follow не открывается
    for (int i = 0; i < 100; i++) {
        tick(20);
    }
    CHECK(!m.follow_open() && m.stats().follow_opened == 0 && m.state() == wake_logic::State::Armed);
    // тап = стоп: abort серверу, музыку выбрасывает session
    d.on_press(t + 10);
    const dialog::Out stop = d.on_release(t + 70);
    CHECK(stop.music_stop && stop.abort);
}
