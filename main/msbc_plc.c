// mSBC の受信で、Bluedroid が欠損補間（PLC）で作った音を見分ける。
//
// Bluedroid（bta_ag_co.c の bta_ag_decode_msbc_frame）は、誤り・受信なしのパケットと、復号できなかったフレームの
// 代わりに、無音のフレーム（sbc_plc_zero_signal_frame）を復号してから補間し、その PCM をアプリに渡す
// （btc_hf_incoming_data_cb_to_app）。復号とアプリへの受け渡しは、同じタスクで続けて呼ばれるので、
// 最後に復号器に渡されたのが無音のフレームなら、次に渡される PCM は補間で作ったもの。
// 復号器が「データが足りない」で終わったときは、何も渡さず、補間もしない（受信の見張りは、届く量で気づく）。
//
// IDF は書き換えず、OI_CODEC_SBC_DecodeFrame をリンク時に差し替える（main/CMakeLists.txt の --wrap）。
// 同じファイルの中からの呼び出しは差し替わらないので、IDF を上げたら、OI_CODEC_SBC_DecodeFrame が
// bta_ag_co.c から呼ばれていること、補間の前に sbc_plc_zero_signal_frame を復号していることを確かめる。

#include <stdbool.h>

#include "bt_diag.h"
#include "msbc_plc.h"
#include "oi_codec_sbc.h"
#include "sbc_plc.h"

static bool s_plc;  // Bluetooth のタスクだけが読み書きする

OI_STATUS __real_OI_CODEC_SBC_DecodeFrame(OI_CODEC_SBC_DECODER_CONTEXT* context, const OI_BYTE** frameData,
                                          OI_UINT32* frameBytes, OI_INT16* pcmData, OI_UINT32* pcmBytes);

OI_STATUS __wrap_OI_CODEC_SBC_DecodeFrame(OI_CODEC_SBC_DECODER_CONTEXT* context, const OI_BYTE** frameData,
                                          OI_UINT32* frameBytes, OI_INT16* pcmData, OI_UINT32* pcmBytes) {
    const OI_BYTE* in = *frameData;
    OI_UINT32 in_len = *frameBytes;
    bool plc = in == (const OI_BYTE*)sbc_plc_zero_signal_frame();
    s_plc = plc;
    OI_STATUS r = __real_OI_CODEC_SBC_DecodeFrame(context, frameData, frameBytes, pcmData, pcmBytes);
    bt_diag_decoded(in, in_len, in_len - *frameBytes, (int)r, plc);
    return r;
}

bool msbc_plc_take(void) {
    bool p = s_plc;
    s_plc = false;
    return p;
}
