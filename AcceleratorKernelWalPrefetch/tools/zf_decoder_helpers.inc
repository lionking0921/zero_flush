/*
    ZeroFlush zf-decoder 字节读助手（DRAM 未对齐安全，HLS 可综合）
    均以绝对字节偏移访问 ap_uint<32>* buf（LE 字节流）。前缀 zf_ 避免与参考符号冲突。
*/
// 读 1 字节
inline ap_uint<8> zf_ld_byte(const ap_uint<32>* buf, ap_uint<64> off)
{
    ap_uint<32> wd = buf[off.range(63, 2)];
    switch (off.range(1, 0))
    {
    case 0:  return wd.range(7, 0);
    case 1:  return wd.range(15, 8);
    case 2:  return wd.range(23, 16);
    default: return wd.range(31, 24);
    }
}
// 读 4 字节（LE）
inline ap_uint<32> zf_ld_u32(const ap_uint<32>* buf, ap_uint<64> off)
{
    ap_uint<62> wi = off.range(63, 2);
    ap_uint<32> w0 = buf[wi];
    ap_uint<32> w1 = buf[wi + 1];
    switch (off.range(1, 0))
    {
    case 0:  return w0;
    case 1:  return (w0 >> 8) | (w1 << 24);
    case 2:  return (w0 >> 16) | (w1 << 16);
    default: return (w0 >> 24) | (w1 << 8);
    }
}
// 读 8 字节（LE）
inline ap_uint<64> zf_rd_u64(const ap_uint<32>* buf, ap_uint<64> off)
{
    ap_uint<32> lo = zf_ld_u32(buf, off);
    ap_uint<32> hi = zf_ld_u32(buf, off + 4);
    ap_uint<64> r;
    r.range(31, 0) = lo;
    r.range(63, 32) = hi;
    return r;
}
// 读 varint32（游标前进其所占字节数）
inline ap_uint<32> zf_rd_varint32(const ap_uint<32>* buf, ap_uint<64>& off)
{
    ap_uint<32> result = 0;
    for (ap_uint<3> i = 0; i < 5; i++)
    {
        ap_uint<8> b = zf_ld_byte(buf, off);
        off += 1;
        result |= (ap_uint<32>)(b & 0x7f) << (i * 7);
        if ((b & 0x80) == 0) break;
    }
    return result;
}

/*
    ZeroFlush WAL 顺序窗预取原语 —— (里程碑 2 新增)
    把 decoder 对 WAL 区（字节 [0,file_size)）的读取从"逐记录随机细读帧"聚成
    "整窗一次线性顺序读入片上 win[] + 本地取字节"。窗 WIN=1024B=256×ap_uint<32>。
    仅对 WAL 区地址调用；slim 区顺序读仍走 zf_ld_* 直读。
*/
// 整窗线性读入：把 [aligned_base, aligned_base+1024) 一次顺序读入 win[]（连续地址 → 宽线性突发）
inline void zfwin_load(const ap_uint<32>* buf, ap_uint<32> win[256], ap_uint<64>& win_base, bool& win_valid, ap_uint<64> aligned_base)
{
    for (ap_uint<9> j = 0; j < 256; j++)
    {
        win[j] = buf[(aligned_base >> 2) + (ap_uint<64>)j];
    }
    win_base = aligned_base;
    win_valid = true;
}
// 确保 addr 落在已载窗内（miss → 整窗载入）
inline void zfwin_ensure(const ap_uint<32>* buf, ap_uint<32> win[256], ap_uint<64>& win_base, bool& win_valid, ap_uint<64> addr)
{
    if (win_valid == false || addr < win_base || addr >= win_base + 1024)
    {
        zfwin_load(buf, win, win_base, win_valid, addr & (~(ap_uint<64>)1023));
    }
}
// 读 1 字节（经窗，LE）
inline ap_uint<8> zfwin_u8(const ap_uint<32>* buf, ap_uint<32> win[256], ap_uint<64>& win_base, bool& win_valid, ap_uint<64> addr)
{
    zfwin_ensure(buf, win, win_base, win_valid, addr);
    ap_uint<10> rel = (ap_uint<10>)(addr - win_base);   // 0..1023
    ap_uint<32> wd = win[rel.range(9, 2)];
    switch (rel.range(1, 0))
    {
    case 0:  return wd.range(7, 0);
    case 1:  return wd.range(15, 8);
    case 2:  return wd.range(23, 16);
    default: return wd.range(31, 24);
    }
}
// 读 4 字节（LE，经窗）
inline ap_uint<32> zfwin_u32(const ap_uint<32>* buf, ap_uint<32> win[256], ap_uint<64>& win_base, bool& win_valid, ap_uint<64> addr)
{
    ap_uint<8> b0 = zfwin_u8(buf, win, win_base, win_valid, addr);
    ap_uint<8> b1 = zfwin_u8(buf, win, win_base, win_valid, addr + 1);
    ap_uint<8> b2 = zfwin_u8(buf, win, win_base, win_valid, addr + 2);
    ap_uint<8> b3 = zfwin_u8(buf, win, win_base, win_valid, addr + 3);
    ap_uint<32> r;
    r.range(7, 0)   = b0;
    r.range(15, 8)  = b1;
    r.range(23, 16) = b2;
    r.range(31, 24) = b3;
    return r;
}
// 读 [addr, addr+16) 整 16B（经窗；固定位段拼装，可综合）
inline ap_uint<128> zfwin_read16(const ap_uint<32>* buf, ap_uint<32> win[256], ap_uint<64>& win_base, bool& win_valid, ap_uint<64> addr)
{
    ap_uint<32> e0 = zfwin_u32(buf, win, win_base, win_valid, addr);
    ap_uint<32> e1 = zfwin_u32(buf, win, win_base, win_valid, addr + 4);
    ap_uint<32> e2 = zfwin_u32(buf, win, win_base, win_valid, addr + 8);
    ap_uint<32> e3 = zfwin_u32(buf, win, win_base, win_valid, addr + 12);
    ap_uint<128> t;
    t.range(31, 0) = e0;
    t.range(63, 32) = e1;
    t.range(95, 64) = e2;
    t.range(127, 96) = e3;
    return t;
}
