package com.freecrane.adrenotest

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import android.view.View
import kotlin.math.abs
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min

// The on-screen Xbox 360 controller. In play mode it draws the controls, follows several fingers at once and reports the
// controller state to PadOut; touches that miss every control are left to the game. In edit mode the controls can be
// dragged, resized and hidden.
class PadView(ctx: Context, private val edit: Boolean, private val out: PadOut?) : View(ctx) {
    private val app: Context = HostApp.instance
    private val els = Pad.load(app)
    private val bound = HashMap<Int, Pad.El>()
    private val pressed = HashSet<String>()
    private val sx = HashMap<String, Float>()
    private val sy = HashMap<String, Float>()
    private var hatX = 0f
    private var hatY = 0f
    private var selected: Pad.El? = null
    private var dragDx = 0f
    private var dragDy = 0f
    private var opacity = Pad.opacity(app)
    private val haptic = Pad.haptics(app)
    var onDone: (() -> Unit)? = null

    private val fill = Paint(Paint.ANTI_ALIAS_FLAG)
    private val line = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE }
    private val txt = Paint(Paint.ANTI_ALIAS_FLAG).apply { textAlign = Paint.Align.CENTER; isFakeBoldText = true }
    private val tmp = RectF()

    init { isFocusable = false; isFocusableInTouchMode = false }

    private fun unit() = min(width, height).toFloat()
    private fun cx(e: Pad.El) = e.x * width
    private fun cy(e: Pad.El) = e.y * height
    private fun rad(e: Pad.El) = e.size * unit() / 2f

    private fun hit(e: Pad.El, px: Float, py: Float): Boolean {
        val dx = px - cx(e); val dy = py - cy(e)
        return if (e.kind == "shoulder") abs(dx) <= rad(e) * 1.1f && abs(dy) <= rad(e) * 0.62f
        else hypot(dx, dy) <= rad(e) * (if (e.kind == "small") 1.35f else 1.15f)
    }

    private fun colorOf(e: Pad.El): Int = when (e.id) {
        "a" -> 0xFF4CAF50.toInt(); "b" -> 0xFFE53935.toInt(); "x" -> 0xFF1E88E5.toInt(); "y" -> 0xFFFDD835.toInt()
        else -> 0xFFFFFFFF.toInt()
    }

    // ---- drawing ----

    override fun onDraw(c: Canvas) {
        val a = opacity / 100f
        if (edit) {
            c.drawColor(0xFF10141B.toInt())
            txt.color = 0x66FFFFFF; txt.textSize = unit() * 0.035f
            c.drawText("Drag a control to move it. Tap it to pick it, then use the buttons at the top.", width / 2f, height * 0.5f, txt)
        }
        for (e in els) {
            if (!e.vis && !edit) continue
            val on = pressed.contains(e.id)
            val dim = if (e.vis) 1f else 0.3f
            fill.color = colorOf(e)
            fill.alpha = (255 * a * dim * (if (on) 0.75f else 0.28f)).toInt()
            line.color = 0xFFFFFFFF.toInt()
            line.strokeWidth = unit() * 0.004f + 1f
            line.alpha = (255 * a * dim * 0.9f).toInt()
            val x = cx(e); val y = cy(e); val r = rad(e)
            when (e.kind) {
                "stick" -> {
                    c.drawCircle(x, y, r, fill); c.drawCircle(x, y, r, line)
                    val kx = x + (sx[e.id] ?: 0f) * r * 0.6f; val ky = y + (sy[e.id] ?: 0f) * r * 0.6f
                    fill.color = 0xFFFFFFFF.toInt(); fill.alpha = (255 * a * dim * 0.55f).toInt()
                    c.drawCircle(kx, ky, r * 0.4f, fill); c.drawCircle(kx, ky, r * 0.4f, line)
                }
                "dpad" -> {
                    val t = r * 0.36f
                    fill.color = 0xFFFFFFFF.toInt(); fill.alpha = (255 * a * dim * 0.28f).toInt()
                    tmp.set(x - r, y - t, x + r, y + t); c.drawRoundRect(tmp, t * 0.4f, t * 0.4f, fill); c.drawRoundRect(tmp, t * 0.4f, t * 0.4f, line)
                    tmp.set(x - t, y - r, x + t, y + r); c.drawRoundRect(tmp, t * 0.4f, t * 0.4f, fill); c.drawRoundRect(tmp, t * 0.4f, t * 0.4f, line)
                    fill.alpha = (255 * a * dim * 0.75f).toInt()
                    if (hatX < 0) { tmp.set(x - r, y - t, x - t, y + t); c.drawRect(tmp, fill) }
                    if (hatX > 0) { tmp.set(x + t, y - t, x + r, y + t); c.drawRect(tmp, fill) }
                    if (hatY < 0) { tmp.set(x - t, y - r, x + t, y - t); c.drawRect(tmp, fill) }
                    if (hatY > 0) { tmp.set(x - t, y + t, x + t, y + r); c.drawRect(tmp, fill) }
                }
                "shoulder" -> {
                    tmp.set(x - r, y - r * 0.5f, x + r, y + r * 0.5f)
                    c.drawRoundRect(tmp, r * 0.3f, r * 0.3f, fill); c.drawRoundRect(tmp, r * 0.3f, r * 0.3f, line)
                    label(c, e, x, y, r * 0.5f, a * dim)
                }
                else -> {
                    c.drawCircle(x, y, r, fill); c.drawCircle(x, y, r, line)
                    label(c, e, x, y, if (e.kind == "small") r * 0.42f else r * 0.9f, a * dim)
                }
            }
            if (edit && e === selected) {
                line.color = 0xFF4FC3F7.toInt(); line.alpha = 255; line.strokeWidth = unit() * 0.006f + 2f
                val rr = if (e.kind == "shoulder") r * 1.15f else r * 1.25f
                tmp.set(x - rr, y - (if (e.kind == "shoulder") r * 0.62f else rr), x + rr, y + (if (e.kind == "shoulder") r * 0.62f else rr))
                c.drawRoundRect(tmp, rr * 0.2f, rr * 0.2f, line)
            }
        }
        if (edit) drawBar(c)
    }

    private fun label(c: Canvas, e: Pad.El, x: Float, y: Float, size: Float, a: Float) {
        if (e.label.isEmpty()) return
        txt.color = 0xFFFFFFFF.toInt(); txt.alpha = (255 * a).toInt(); txt.textSize = size
        c.drawText(e.label, x, y + size * 0.35f, txt)
    }

    // ---- edit bar ----

    private val barLabels = arrayOf("Smaller", "Bigger", "Hide / show", "Reset all", "Done")
    private fun barRect(i: Int): RectF {
        val w = width / 5.6f; val h = unit() * 0.11f
        val left = (width - w * 5.2f) / 2f + i * w * 1.05f
        return RectF(left, unit() * 0.03f, left + w, unit() * 0.03f + h)
    }

    private fun drawBar(c: Canvas) {
        for (i in barLabels.indices) {
            val r = barRect(i)
            fill.color = if (i == 4) 0xFF2E7D32.toInt() else 0xFF2A313C.toInt(); fill.alpha = 255
            c.drawRoundRect(r, 14f, 14f, fill)
            txt.color = 0xFFFFFFFF.toInt(); txt.alpha = 255; txt.textSize = r.height() * 0.34f
            c.drawText(barLabels[i], r.centerX(), r.centerY() + r.height() * 0.12f, txt)
        }
    }

    private fun barTap(px: Float, py: Float): Boolean {
        for (i in barLabels.indices) {
            if (!barRect(i).contains(px, py)) continue
            val s = selected
            when (i) {
                0 -> if (s != null) s.size = max(0.04f, s.size * 0.9f)
                1 -> if (s != null) s.size = min(0.8f, s.size * 1.1f)
                2 -> if (s != null) s.vis = !s.vis
                3 -> { Pad.resetLayout(app); els.clear(); els.addAll(Pad.defaults()); selected = null }
                4 -> { save(); onDone?.invoke() }
            }
            invalidate()
            return true
        }
        return false
    }

    fun save() { Pad.save(app, els) }

    // ---- touches ----

    private fun hitTop(px: Float, py: Float): Pad.El? {
        for (i in els.indices.reversed()) {
            val e = els[i]
            if ((e.vis || edit) && hit(e, px, py)) return e
        }
        return null
    }

    override fun onTouchEvent(ev: MotionEvent): Boolean {
        if (edit) return editTouch(ev)
        var took = false
        when (ev.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                val i = ev.actionIndex
                val e = hitTop(ev.getX(i), ev.getY(i))
                if (e != null) {
                    bound[ev.getPointerId(i)] = e
                    pressEl(e, ev.getX(i), ev.getY(i), true)
                    took = true
                }
            }
            MotionEvent.ACTION_MOVE -> {
                for (i in 0 until ev.pointerCount) {
                    val e = bound[ev.getPointerId(i)] ?: continue
                    pressEl(e, ev.getX(i), ev.getY(i), false)
                }
                took = bound.isNotEmpty()
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> {
                val i = ev.actionIndex
                val e = bound.remove(ev.getPointerId(i))
                if (e != null) { releaseEl(e); took = true }
            }
            MotionEvent.ACTION_CANCEL -> {
                for (e in bound.values.toList()) releaseEl(e)
                bound.clear()
            }
        }
        push()
        invalidate()
        return true          // the controller covers the whole screen: the game gets no touches while it is on
    }

    private fun pressEl(e: Pad.El, px: Float, py: Float, first: Boolean) {
        when (e.kind) {
            "stick" -> {
                val r = rad(e)
                var dx = (px - cx(e)) / r; var dy = (py - cy(e)) / r
                val m = hypot(dx, dy)
                if (m > 1f) { dx /= m; dy /= m }
                sx[e.id] = dx; sy[e.id] = dy
            }
            "dpad" -> {
                val r = rad(e)
                val dx = (px - cx(e)) / r; val dy = (py - cy(e)) / r
                hatX = if (dx > 0.28f) 1f else if (dx < -0.28f) -1f else 0f
                hatY = if (dy > 0.28f) 1f else if (dy < -0.28f) -1f else 0f
            }
            else -> if (pressed.add(e.id) && first && haptic) performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY)
        }
    }

    private fun releaseEl(e: Pad.El) {
        when (e.kind) {
            "stick" -> { sx[e.id] = 0f; sy[e.id] = 0f }
            "dpad" -> { hatX = 0f; hatY = 0f }
            else -> pressed.remove(e.id)
        }
    }

    private fun push() {
        out?.update(pressed, sx["ls"] ?: 0f, sy["ls"] ?: 0f, sx["rs"] ?: 0f, sy["rs"] ?: 0f, hatX, hatY)
    }

    private fun editTouch(ev: MotionEvent): Boolean {
        val px = ev.x; val py = ev.y
        when (ev.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                if (barTap(px, py)) return true
                val e = hitTop(px, py)
                selected = e
                if (e != null) { dragDx = cx(e) - px; dragDy = cy(e) - py }
                invalidate()
            }
            MotionEvent.ACTION_MOVE -> {
                val e = selected
                if (e != null && width > 0 && height > 0) {
                    e.x = ((px + dragDx) / width).coerceIn(0.02f, 0.98f)
                    e.y = ((py + dragDy) / height).coerceIn(0.02f, 0.98f)
                    invalidate()
                }
            }
        }
        return true
    }
}
