package dev.radek.conventor

/** Bounds decode and cache costs for artwork in a paginated library. */
internal object ArtworkSizing {
    const val MAX_TARGET_PIXELS = 512

    fun target(pixels: Int): Int = pixels.coerceIn(1, MAX_TARGET_PIXELS)

    fun sample(width: Int, height: Int, requested: Int): Int {
        val longest = maxOf(width, height)
        if (longest <= 0) return 1
        val target = target(requested)
        var sample = 1
        while (sample < 512 && longest / (sample * 2) >= target) sample *= 2
        return sample
    }

    fun scaled(width: Int, height: Int, requested: Int): Pair<Int, Int> {
        val longest = maxOf(width, height)
        require(width > 0 && height > 0) { "Invalid artwork dimensions" }
        val target = target(requested)
        if (longest <= target) return width to height
        return (width.toLong() * target / longest).coerceAtLeast(1).toInt() to
            (height.toLong() * target / longest).coerceAtLeast(1).toInt()
    }
}
