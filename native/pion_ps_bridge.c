#include "moonbit.h"
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

void pion_ps_stdout(const uint8_t *s, int64_t sl) {
  if (s && sl > 0) fwrite(s, 1, (size_t)sl, stdout);
  fputc('\n', stdout);
  fflush(stdout);
}

extern moonbit_bytes_t pion_mb_bytes_from_raw(const uint8_t *ptr, int64_t len);

extern void ps_tray_note(moonbit_bytes_t title, moonbit_bytes_t text);
extern void ps_set_menu(moonbit_bytes_t lines);
extern void ps_log(moonbit_bytes_t line);
extern moonbit_bytes_t ps_read_file(moonbit_bytes_t path);
extern moonbit_bytes_t ps_picture_jpeg(moonbit_bytes_t pic, int32_t quality);
extern moonbit_bytes_t ps_picture_oriented(moonbit_bytes_t pic, int32_t turns, int32_t flipped, int32_t shown);
extern int32_t ps_write_file(moonbit_bytes_t path, moonbit_bytes_t data);
extern moonbit_bytes_t ps_save_dialog(int32_t wid, moonbit_bytes_t dir, moonbit_bytes_t name);
extern moonbit_bytes_t ps_pick_folder(int32_t wid, moonbit_bytes_t dir);
extern void ps_reveal(moonbit_bytes_t path);
extern moonbit_bytes_t ps_popup_menu(int32_t wid, moonbit_bytes_t lines);
extern int32_t ps_ocr(int32_t id, moonbit_bytes_t png);
extern moonbit_bytes_t ps_ocr_now(moonbit_bytes_t png);
extern int32_t ps_rec_export(moonbit_bytes_t dest_path, int32_t fmt, int32_t quality, int32_t max_side, int32_t gif_fps);
extern int32_t ps_rec_reuse(moonbit_bytes_t src_path, moonbit_bytes_t dst_path);
extern moonbit_bytes_t ps_update_path(moonbit_bytes_t name);
extern int32_t ps_fetch(moonbit_bytes_t url, moonbit_bytes_t path, moonbit_bytes_t sha256, moonbit_bytes_t agent);
extern int32_t ps_release_signed(moonbit_bytes_t version, moonbit_bytes_t setup_sha, moonbit_bytes_t exe_sha, moonbit_bytes_t sig);
extern int32_t ps_install_cli(moonbit_bytes_t version);
extern void ps_install_sync_meta(moonbit_bytes_t version);
extern int32_t ps_swap_exe(moonbit_bytes_t path, moonbit_bytes_t sha256);
extern void ps_open_url(moonbit_bytes_t url);
extern int32_t ps_tr_render(int32_t id, moonbit_bytes_t pic, moonbit_bytes_t spec, int32_t zh);
extern int32_t ps_http(int32_t id, moonbit_bytes_t verb, moonbit_bytes_t url, moonbit_bytes_t headers, moonbit_bytes_t body,
                       int32_t timeout_ms);

static inline moonbit_bytes_t mb_from(const uint8_t *p, int64_t n) {
  return pion_mb_bytes_from_raw(p, n);
}

void pion_ps_tray_note(const uint8_t *t, int64_t tl, const uint8_t *m, int64_t ml) {
  moonbit_bytes_t bt = mb_from(t, tl), bm = mb_from(m, ml);
  ps_tray_note(bt, bm);
  moonbit_decref(bt);
  moonbit_decref(bm);
}

void pion_ps_set_menu(const uint8_t *s, int64_t sl) {
  moonbit_bytes_t bs = mb_from(s, sl);
  ps_set_menu(bs);
  moonbit_decref(bs);
}

void pion_ps_log(const uint8_t *s, int64_t sl) {
  moonbit_bytes_t bs = mb_from(s, sl);
  ps_log(bs);
  moonbit_decref(bs);
}

void *pion_ps_read_file(const uint8_t *p, int64_t pl) {
  moonbit_bytes_t bp = mb_from(p, pl);
  moonbit_bytes_t out = ps_read_file(bp);
  moonbit_decref(bp);
  return out;
}

void *pion_ps_picture_jpeg(const uint8_t *p, int64_t pl, int32_t quality) {
  moonbit_bytes_t bp = mb_from(p, pl);
  moonbit_bytes_t out = ps_picture_jpeg(bp, quality);
  moonbit_decref(bp);
  return out;
}

void *pion_ps_picture_oriented(const uint8_t *p, int64_t pl, int32_t turns, int32_t flipped, int32_t shown) {
  moonbit_bytes_t bp = mb_from(p, pl);
  moonbit_bytes_t out = ps_picture_oriented(bp, turns, flipped, shown);
  moonbit_decref(bp);
  return out;
}

int32_t pion_ps_write_file(const uint8_t *p, int64_t pl, const uint8_t *d, int64_t dl) {
  moonbit_bytes_t bp = mb_from(p, pl), bd = mb_from(d, dl);
  int32_t r = ps_write_file(bp, bd);
  moonbit_decref(bp);
  moonbit_decref(bd);
  return r;
}

void *pion_ps_save_dialog(int32_t wid, const uint8_t *dir, int64_t dl, const uint8_t *name, int64_t nl) {
  moonbit_bytes_t bd = mb_from(dir, dl), bn = mb_from(name, nl);
  moonbit_bytes_t out = ps_save_dialog(wid, bd, bn);
  moonbit_decref(bd);
  moonbit_decref(bn);
  return out;
}

void *pion_ps_pick_folder(int32_t wid, const uint8_t *dir, int64_t dl) {
  moonbit_bytes_t bd = mb_from(dir, dl);
  moonbit_bytes_t out = ps_pick_folder(wid, bd);
  moonbit_decref(bd);
  return out;
}

void pion_ps_reveal(const uint8_t *p, int64_t pl) {
  moonbit_bytes_t bp = mb_from(p, pl);
  ps_reveal(bp);
  moonbit_decref(bp);
}

void *pion_ps_popup_menu(int32_t wid, const uint8_t *s, int64_t sl) {
  moonbit_bytes_t bs = mb_from(s, sl);
  moonbit_bytes_t out = ps_popup_menu(wid, bs);
  moonbit_decref(bs);
  return out;
}

int32_t pion_ps_ocr(int32_t id, const uint8_t *png, int64_t pl) {
  moonbit_bytes_t bp = mb_from(png, pl);
  int32_t r = ps_ocr(id, bp);
  moonbit_decref(bp);
  return r;
}

void *pion_ps_ocr_now(const uint8_t *png, int64_t pl) {
  moonbit_bytes_t bp = mb_from(png, pl);
  moonbit_bytes_t out = ps_ocr_now(bp);
  moonbit_decref(bp);
  return out;
}

int32_t pion_ps_rec_export(const uint8_t *dest, int64_t dl, int32_t fmt, int32_t quality, int32_t max_side, int32_t gif_fps) {
  moonbit_bytes_t bd = mb_from(dest, dl);
  int32_t r = ps_rec_export(bd, fmt, quality, max_side, gif_fps);
  moonbit_decref(bd);
  return r;
}

int32_t pion_ps_rec_reuse(const uint8_t *src, int64_t sl, const uint8_t *dst, int64_t dl) {
  moonbit_bytes_t bs = mb_from(src, sl), bd = mb_from(dst, dl);
  int32_t r = ps_rec_reuse(bs, bd);
  moonbit_decref(bs);
  moonbit_decref(bd);
  return r;
}

void *pion_ps_update_path(const uint8_t *name, int64_t nl) {
  moonbit_bytes_t bn = mb_from(name, nl);
  moonbit_bytes_t out = ps_update_path(bn);
  moonbit_decref(bn);
  return out;
}

int32_t pion_ps_fetch(
  const uint8_t *u, int64_t ul,
  const uint8_t *p, int64_t pl,
  const uint8_t *s, int64_t sl,
  const uint8_t *a, int64_t al
) {
  moonbit_bytes_t bu = mb_from(u, ul), bp = mb_from(p, pl), bs = mb_from(s, sl), ba = mb_from(a, al);
  int32_t r = ps_fetch(bu, bp, bs, ba);
  moonbit_decref(bu);
  moonbit_decref(bp);
  moonbit_decref(bs);
  moonbit_decref(ba);
  return r;
}

int32_t pion_ps_release_signed(
  const uint8_t *v, int64_t vl,
  const uint8_t *ss, int64_t ssl,
  const uint8_t *es, int64_t esl,
  const uint8_t *sg, int64_t sgl
) {
  moonbit_bytes_t bv = mb_from(v, vl), bss = mb_from(ss, ssl), bes = mb_from(es, esl), bsg = mb_from(sg, sgl);
  int32_t r = ps_release_signed(bv, bss, bes, bsg);
  moonbit_decref(bv);
  moonbit_decref(bss);
  moonbit_decref(bes);
  moonbit_decref(bsg);
  return r;
}

int32_t pion_ps_install_cli(const uint8_t *v, int64_t vl) {
  moonbit_bytes_t bv = mb_from(v, vl);
  int32_t r = ps_install_cli(bv);
  moonbit_decref(bv);
  return r;
}

void pion_ps_install_sync_meta(const uint8_t *v, int64_t vl) {
  moonbit_bytes_t bv = mb_from(v, vl);
  ps_install_sync_meta(bv);
  moonbit_decref(bv);
}

int32_t pion_ps_swap_exe(const uint8_t *p, int64_t pl, const uint8_t *s, int64_t sl) {
  moonbit_bytes_t bp = mb_from(p, pl), bs = mb_from(s, sl);
  int32_t r = ps_swap_exe(bp, bs);
  moonbit_decref(bp);
  moonbit_decref(bs);
  return r;
}

void pion_ps_open_url(const uint8_t *u, int64_t ul) {
  moonbit_bytes_t bu = mb_from(u, ul);
  ps_open_url(bu);
  moonbit_decref(bu);
}

int32_t pion_ps_tr_render(int32_t id, const uint8_t *p, int64_t pl, const uint8_t *s, int64_t sl, int32_t zh) {
  moonbit_bytes_t bp = mb_from(p, pl), bs = mb_from(s, sl);
  int32_t r = ps_tr_render(id, bp, bs, zh);
  moonbit_decref(bp);
  moonbit_decref(bs);
  return r;
}

int32_t pion_ps_http(
  int32_t id,
  const uint8_t *v, int64_t vl,
  const uint8_t *u, int64_t ul,
  const uint8_t *h, int64_t hl,
  const uint8_t *b, int64_t bl,
  int32_t timeout_ms
) {
  moonbit_bytes_t bv = mb_from(v, vl), bu = mb_from(u, ul), bh = mb_from(h, hl), bb = mb_from(b, bl);
  int32_t r = ps_http(id, bv, bu, bh, bb, timeout_ms);
  moonbit_decref(bv);
  moonbit_decref(bu);
  moonbit_decref(bh);
  moonbit_decref(bb);
  return r;
}
