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
