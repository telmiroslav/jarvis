#include "mww_core.h"

#include <new>
#include <stdio.h>
#include <string.h>

#include "frontend.h"
#include "frontend_util.h"

#include "tensorflow/lite/micro/micro_allocator.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"

#include "time_util.h"

namespace jarvis {
namespace mww {

namespace {

// Нужна только таблица состояний (6 переменных у модели), сами буферы состояния лежат в арене тензоров. Отдельный кусочек
// под нужды MicroAllocator и таблицы переменных; запас с избытком.
constexpr size_t kVarArenaBytes = 2048;
constexpr int kMaxVariables = 16;

void set_err(char *err, size_t cap, const char *msg)
{
    if (err && cap > 0) {
        snprintf(err, cap, "%s", msg);
    }
}

}  // namespace

struct Core::Impl {
    FrontendState fe;
    bool fe_ok = false;

    tflite::MicroMutableOpResolver<kOpCount> resolver;
    alignas(16) uint8_t var_arena[kVarArenaBytes];
    tflite::MicroAllocator *var_alloc = nullptr;
    tflite::MicroResourceVariables *vars = nullptr;

    uint8_t *arena = nullptr;
    size_t arena_len = 0;
    tflite::MicroInterpreter *interp = nullptr;
    alignas(16) uint8_t interp_mem[sizeof(tflite::MicroInterpreter)];

    mww_logic::InputQuant quant;
    int stride = 0;
    int slot = 0;                     // сколько признаков уже ждёт запуска модели
    int8_t pending[mww_logic::kMaxStride * mww_logic::kChannels];
    uint16_t last_raw[mww_logic::kChannels];
    int8_t last_q[mww_logic::kChannels];
    float out_scale = 0.0f;
    int32_t out_zp = 0;
    bool out_unsigned = true;
};

Core::~Core() { deinit(); }

void Core::deinit()
{
    if (!impl_) {
        return;
    }
    Impl *im = impl_;
    impl_ = nullptr;
    if (im->interp) {
        im->interp->~MicroInterpreter();
        im->interp = nullptr;
    }
    if (im->fe_ok) {
        FrontendFreeStateContents(&im->fe);
        im->fe_ok = false;
    }
    if (im->arena && ci_.release) {
        ci_.release(im->arena);
    }
    im->~Impl();
    if (ci_.release) {
        ci_.release(im);
    }
}

bool Core::init(const CoreInit &ci, char *err, size_t err_cap)
{
    deinit();
    ci_ = ci;
    info_ = CoreInfo();
    if (!ci.model || ci.model_len < 16 || !ci.alloc || !ci.release) {
        set_err(err, err_cap, "bad model or allocator");
        return false;
    }
    if (memcmp(ci.model + 4, "TFL3", 4) != 0) {
        set_err(err, err_cap, "the model is not a TFLite flatbuffer (no TFL3 mark)");
        return false;
    }
    const tflite::Model *model = tflite::GetModel(ci.model);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        set_err(err, err_cap, "the model schema version is not supported");
        return false;
    }

    void *mem = ci.alloc(sizeof(Impl), 16);
    if (!mem) {
        set_err(err, err_cap, "no memory for the engine object");
        return false;
    }
    Impl *im = new (mem) Impl();
    impl_ = im;
    info_.model_bytes = ci.model_len;
    info_.object_bytes = sizeof(Impl);

    // Операции только те, что нужны модели (список проверен по самому файлу: tools в отчёте, тест test_mww_core)
    bool ops_ok = true;
    ops_ok = ops_ok && im->resolver.AddCallOnce() == kTfLiteOk;
    ops_ok = ops_ok && im->resolver.AddVarHandle() == kTfLiteOk;
    ops_ok = ops_ok && im->resolver.AddReshape() == kTfLiteOk;
    ops_ok = ops_ok && im->resolver.AddReadVariable() == kTfLiteOk;
    ops_ok = ops_ok && im->resolver.AddConcatenation() == kTfLiteOk;
    ops_ok = ops_ok && im->resolver.AddStridedSlice() == kTfLiteOk;
    ops_ok = ops_ok && im->resolver.AddAssignVariable() == kTfLiteOk;
    ops_ok = ops_ok && im->resolver.AddConv2D() == kTfLiteOk;
    ops_ok = ops_ok && im->resolver.AddDepthwiseConv2D() == kTfLiteOk;
    ops_ok = ops_ok && im->resolver.AddSplitV() == kTfLiteOk;
    ops_ok = ops_ok && im->resolver.AddFullyConnected() == kTfLiteOk;
    ops_ok = ops_ok && im->resolver.AddLogistic() == kTfLiteOk;
    ops_ok = ops_ok && im->resolver.AddQuantize() == kTfLiteOk;
    if (!ops_ok) {
        set_err(err, err_cap, "cannot register the TFLite operations");
        deinit();
        return false;
    }

    // Арена тензоров: начинаем с размера из манифеста; если модели не хватило, берём больше (ядра esp-nn просят больше
    // эталонных). Таблица переменных каждый раз новая: после неудачной попытки в ней остались бы указатели в старую арену.
    bool loaded = false;
    for (int attempt = 0; attempt < mww_logic::kArenaAttempts && !loaded; attempt++) {
        const size_t len = mww_logic::arena_attempt_bytes(ci.manifest.arena_bytes, attempt);
        uint8_t *arena = static_cast<uint8_t *>(ci.alloc(len, 16));
        if (!arena) {
            continue;   // такого куска нет, возможно, поменьше найдётся на следующей попытке (она больше: тогда тоже не найдётся)
        }
        im->var_alloc = tflite::MicroAllocator::Create(im->var_arena, kVarArenaBytes);
        im->vars = im->var_alloc ? tflite::MicroResourceVariables::Create(im->var_alloc, kMaxVariables) : nullptr;
        if (!im->vars) {
            ci.release(arena);
            set_err(err, err_cap, "cannot create the resource variable table");
            deinit();
            return false;
        }
        tflite::MicroInterpreter *it =
            new (im->interp_mem) tflite::MicroInterpreter(model, im->resolver, arena, len, im->vars);
        if (it->AllocateTensors() == kTfLiteOk) {
            im->interp = it;
            im->arena = arena;
            im->arena_len = len;
            info_.arena_bytes = len;
            info_.arena_used = it->arena_used_bytes();
            info_.arena_attempts = attempt + 1;
            loaded = true;
        } else {
            it->~MicroInterpreter();
            ci.release(arena);
        }
    }
    if (!loaded) {
        set_err(err, err_cap, "AllocateTensors failed (arena too small or an operation is missing)");
        deinit();
        return false;
    }

    // Вход и выход: [1, stride, 40] int8 и [1, 1] uint8 (или int8)
    TfLiteTensor *in = im->interp->input(0);
    TfLiteTensor *out = im->interp->output(0);
    if (!in || !out || im->interp->inputs_size() != 1 || im->interp->outputs_size() != 1) {
        set_err(err, err_cap, "the model must have one input and one output");
        deinit();
        return false;
    }
    if (in->type != kTfLiteInt8 || in->dims->size != 3 || in->dims->data[0] != 1 || in->dims->data[2] != mww_logic::kChannels ||
        in->dims->data[1] < 1 || in->dims->data[1] > mww_logic::kMaxStride) {
        set_err(err, err_cap, "the model input is not int8 [1, stride, 40]");
        deinit();
        return false;
    }
    if ((out->type != kTfLiteUInt8 && out->type != kTfLiteInt8) || out->dims->size != 2 || out->dims->data[0] != 1 ||
        out->dims->data[1] != 1) {
        set_err(err, err_cap, "the model output is not a single uint8/int8 value");
        deinit();
        return false;
    }
    im->stride = in->dims->data[1];
    im->quant = mww_logic::make_input_quant(in->params.scale, in->params.zero_point);
    im->out_scale = out->params.scale;
    im->out_zp = out->params.zero_point;
    im->out_unsigned = (out->type == kTfLiteUInt8);
    if (!(in->params.scale > 0.0f) || !(out->params.scale > 0.0f)) {
        set_err(err, err_cap, "the model tensors have no quantization");
        deinit();
        return false;
    }
    info_.stride = im->stride;
    info_.in_scale = in->params.scale;
    info_.in_zero_point = in->params.zero_point;
    info_.out_scale = out->params.scale;
    info_.out_zero_point = out->params.zero_point;
    info_.out_unsigned = im->out_unsigned;

    // Микрофронтенд с теми же параметрами, что при обучении (mww_logic.h)
    FrontendConfig fc;
    FrontendFillConfigWithDefaults(&fc);
    fc.window.size_ms = mww_logic::kWindowMs;
    fc.window.step_size_ms = mww_logic::kStepMs;
    fc.filterbank.num_channels = mww_logic::kChannels;
    fc.filterbank.lower_band_limit = mww_logic::kLowerBandHz;
    fc.filterbank.upper_band_limit = mww_logic::kUpperBandHz;
    fc.noise_reduction.smoothing_bits = mww_logic::kNoiseSmoothingBits;
    fc.noise_reduction.even_smoothing = mww_logic::kNoiseEvenSmoothing;
    fc.noise_reduction.odd_smoothing = mww_logic::kNoiseOddSmoothing;
    fc.noise_reduction.min_signal_remaining = mww_logic::kNoiseMinSignalRemaining;
    fc.pcan_gain_control.enable_pcan = 1;
    fc.pcan_gain_control.strength = mww_logic::kPcanStrength;
    fc.pcan_gain_control.offset = mww_logic::kPcanOffset;
    fc.pcan_gain_control.gain_bits = mww_logic::kPcanGainBits;
    fc.log_scale.enable_log = 1;
    fc.log_scale.scale_shift = mww_logic::kLogScaleShift;
    if (!FrontendPopulateState(&fc, &im->fe, static_cast<int>(mww_logic::kSampleRate))) {
        set_err(err, err_cap, "cannot create the feature extractor (out of memory?)");
        deinit();
        return false;
    }
    im->fe_ok = true;
    im->slot = 0;
    return true;
}

void Core::reset()
{
    if (!impl_) {
        return;
    }
    Impl *im = impl_;
    FrontendReset(&im->fe);
    im->slot = 0;
    memset(im->pending, 0, sizeof(im->pending));
    if (im->interp) {
        im->interp->Reset();   // состояния потоковых слоёв в ноль, начальный подграф сработает заново
    }
}

const uint16_t *Core::last_raw() const { return impl_ ? impl_->last_raw : nullptr; }

const int8_t *Core::last_quantized() const { return impl_ ? impl_->last_q : nullptr; }

Step Core::step(const int16_t *pcm, size_t n)
{
    Step r;
    if (!impl_ || !pcm || n == 0) {
        return r;
    }
    Impl *im = impl_;
    const auto now = ci_.clock_us;
    const uint32_t t0 = now ? now() : 0;
    size_t read = 0;
    const FrontendOutput fo = FrontendProcessSamples(&im->fe, pcm, n, &read);
    r.consumed = read;
    if (fo.size == 0 || !fo.values) {
        return r;
    }
    if (fo.size != static_cast<size_t>(mww_logic::kChannels)) {
        r.error = true;
        return r;
    }
    int8_t *dst = im->pending + static_cast<size_t>(im->slot) * mww_logic::kChannels;
    for (int i = 0; i < mww_logic::kChannels; i++) {
        dst[i] = mww_logic::quantize_feature(fo.values[i], im->quant);
        im->last_raw[i] = fo.values[i];
        im->last_q[i] = dst[i];
    }
    im->slot++;
    r.feature = true;
    const uint32_t t1 = now ? now() : 0;
    r.fe_us = timeutil::elapsed(t1, t0);   // мкс (метки 32-битные, переполнение через 71 мин не страшно)
    if (im->slot < im->stride) {
        return r;
    }
    im->slot = 0;

    TfLiteTensor *in = im->interp->input(0);
    memcpy(in->data.int8, im->pending, static_cast<size_t>(im->stride) * mww_logic::kChannels);
    const TfLiteStatus st = im->interp->Invoke();
    const uint32_t t2 = now ? now() : 0;
    r.inf_us = timeutil::elapsed(t2, t1);
    if (st != kTfLiteOk) {
        r.error = true;
        return r;
    }
    const TfLiteTensor *out = im->interp->output(0);
    const int32_t q = im->out_unsigned ? static_cast<int32_t>(out->data.uint8[0]) : static_cast<int32_t>(out->data.int8[0]);
    r.prob = mww_logic::dequantize_prob(q, im->out_scale, im->out_zp);
    r.inferred = true;
    return r;
}

}  // namespace mww
}  // namespace jarvis
