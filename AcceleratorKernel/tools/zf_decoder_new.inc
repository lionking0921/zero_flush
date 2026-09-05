/*
    ============================================================
    ZeroFlush zf-decoder —— (实现文本，由 tools/apply_decoder.py 拼入内核)
    替换参考 decoder（LevelDB 完整 SST 读取重放）为面向
    [WAL 段字节] ++ [RocksDB 版 slim 条目区] 的 aux-sort 物化解码器。
    ============================================================
*/
void decoder(ap_uint<32> *buf,  ap_uint<32> kv_sum, ap_uint<40> file_size, hls::stream<fifo_key_meta> &km_stream, hls::stream<fifo_value_slice> &value_stream)
{
    /*
        输入 staged 缓冲布局（host 拼装）：
            bytes[0 .. file_size)         = WAL 段原文（冻结代 (part,gen) 的 zf-wal 段）
            bytes[file_size .. end)       = slim 条目区（kv_sum 条，内部键升序）
        slim 条目（RocksDB 版）：
            varint32(ik_len) internal_key(varint) varint32(16) locator(16B)
            internal_key = user(24) || LE64((seq<<8)|type)，本里程碑 ik_len 固定 32
            locator = part u32 LE | gen u32 LE | wal_offset u64 LE（帧首字节偏移）
        物化 value：从 WAL 帧首 wal_offset 读 24B header（key_len@8, val_len@12），
            value 位于 24+key_len，长 val_len（≤1024，删除标记 val_len=0）。
        输出协议与参考 decoder 逐字一致：流头 SIGNAL；每记录 1×km + 固定 2×512B
            value（零填充）；流尾 MAX_KEY。空 run 只发 SIGNAL+MAX_KEY。
        本里程碑每输入口只含单个 (part,gen) 段；多段/目录表留待后续。
    */
    {
        fifo_key_meta SIGNAL;
        set_signal_key(SIGNAL.key);
        km_stream.write(SIGNAL);
    }

    if (file_size == 0 || kv_sum == 0)
    {
        fifo_key_meta MAX_KEY;
        set_max_key(MAX_KEY.key);
        km_stream.write(MAX_KEY);
        return;
    }

    ap_uint<64> slim_cursor = file_size;  // WAL 区字节长 == slim 区起点

    zf_decoder_main_loop:
    for (ap_uint<32> i = 0; i < kv_sum; i++)
    {
        // ---------- 1) slim：ik_len varint32 ----------
        ap_uint<32> ik_len = zf_rd_varint32(buf, slim_cursor);
        ap_uint<8>  ik_len8 = ik_len.range(7,0);

        // ---------- 2) 组内部键 keystring（c[] 为自然字节镜像）----------
        keystring key;
        key.length = ik_len8;
        // 每个 16B 块整体从 DRAM 读（避免对 128 位做动态位段写）
        ap_uint<2>  nwords = (ik_len8 + 15) >> 4;   // 1..2
        for (ap_uint<2> ci = 0; ci < nwords; ci++)
        {
            ap_uint<64> kb = slim_cursor + (ap_uint<64>)(ci * 16);
            ap_uint<32> w0 = zf_ld_u32(buf, kb);
            ap_uint<32> w1 = zf_ld_u32(buf, kb + 4);
            ap_uint<32> w2 = zf_ld_u32(buf, kb + 8);
            ap_uint<32> w3 = zf_ld_u32(buf, kb + 12);
            ap_uint<128> t;
            t.range(31, 0) = w0;
            t.range(63, 32) = w1;
            t.range(95, 64) = w2;
            t.range(127, 96) = w3;
            key.c[ci] = t;
        }
        slim_cursor += ik_len;  // 前移完整 ik_len 字节（不含 16B 对齐填充）

        // ---------- 3) slim：locator ----------
        ap_uint<32> loc_len = zf_rd_varint32(buf, slim_cursor);  // 期望 16
        // locator = part u32 | gen u32 | wal_offset u64（LE）
        ap_uint<64> wal_offset = zf_rd_u64(buf, slim_cursor + 8);
        slim_cursor += loc_len;   // 前移 locator 载荷（16B）

        // ---------- 4) WAL 帧：物化 value ----------
        ap_uint<64> rec_addr = wal_offset;   // WAL 段在缓冲内从字节 0 起
        ap_uint<32> wkey_len = zf_ld_u32(buf, rec_addr + 8);
        ap_uint<32> val_len  = zf_ld_u32(buf, rec_addr + 12);
        // 帧首 @0：magic u32；@4 cf_id u16；@6 type u8；@7 flags u8；@16 seq u64
        ap_uint<64> vbase = rec_addr + 24 + wkey_len;

        // 先发 km（与参考 decoder 一致：km 先于 value 切片）
        fifo_key_meta km;
        km.key = key;
        km.value_length = val_len.range(VALUE_LENGTH_BITWIDTH - 1, 0);
        km_stream.write(km);

        // 固定 2×512B 切片；value 字节 [0,val_len) 落在 1024B 窗口前部，尾部零填充
        fifo_value_slice sl0;
        fifo_value_slice sl1;
        for (ap_uint<6> k = 0; k < 32; k++) { sl0.c[k] = 0; sl1.c[k] = 0; }
        for (ap_uint<7> w = 0; w < 64; w++)  // 64×16B = 1024B
        {
            ap_uint<64> base = (ap_uint<64>)(w) * 16;
            if (base < val_len)
            {
                ap_uint<64> a = vbase + base;
                ap_uint<32> e0 = zf_ld_u32(buf, a);
                ap_uint<32> e1 = zf_ld_u32(buf, a + 4);
                ap_uint<32> e2 = zf_ld_u32(buf, a + 8);
                ap_uint<32> e3 = zf_ld_u32(buf, a + 12);
                ap_uint<128> t;
                t.range(31, 0) = e0;
                t.range(63, 32) = e1;
                t.range(95, 64) = e2;
                t.range(127, 96) = e3;
                if (w < 32) sl0.c[w.range(4, 0)] = t;
                else        sl1.c[w.range(4, 0)] = t;
            }
        }
        value_stream.write(sl0);
        value_stream.write(sl1);
    }

    {
        fifo_key_meta MAX_KEY;
        set_max_key(MAX_KEY.key);
        km_stream.write(MAX_KEY);
    }
}
