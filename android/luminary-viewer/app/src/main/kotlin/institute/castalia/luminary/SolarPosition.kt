package institute.castalia.luminary

import java.util.Calendar
import java.util.TimeZone
import kotlin.math.abs
import kotlin.math.acos
import kotlin.math.asin
import kotlin.math.cos
import kotlin.math.sin
import kotlin.math.tan

/**
 * Solar altitude and azimuth for a fixed location from the device clock, so
 * the sky tracks the real day continuously and offline -- the same thing the
 * P4 firmware does by computing York's solar position locally rather than
 * waiting for the network. NOAA's low-precision algorithm; well under a
 * degree, far finer than the grading needs.
 */
object SolarPosition {
    // Cape Neddick (Nubble) Light, the scene's anchor.
    private const val LAT = 43.16530
    private const val LON = -70.59110

    data class Sun(val altitudeDeg: Double, val azimuthDeg: Double)

    fun current(): Sun {
        val utc = Calendar.getInstance(TimeZone.getTimeZone("UTC"))
        val year = utc.get(Calendar.YEAR)
        val month = utc.get(Calendar.MONTH) + 1
        val day = utc.get(Calendar.DAY_OF_MONTH)
        val hour = utc.get(Calendar.HOUR_OF_DAY) +
            utc.get(Calendar.MINUTE) / 60.0 + utc.get(Calendar.SECOND) / 3600.0

        // Julian day (Fliegel-Van Flandern) and Julian century.
        val a = (14 - month) / 12
        val y = year + 4800 - a
        val m = month + 12 * a - 3
        val jdn = day + (153 * m + 2) / 5 + 365 * y + y / 4 - y / 100 + y / 400 - 32045
        val jd = jdn + (hour - 12.0) / 24.0
        val t = (jd - 2451545.0) / 36525.0

        // Sun's apparent position (NOAA low-precision series).
        val l0 = norm360(280.46646 + t * (36000.76983 + t * 0.0003032))
        val mAnom = 357.52911 + t * (35999.05029 - t * 0.0001537)
        val e = 0.016708634 - t * (0.000042037 + t * 0.0000001267)
        val mRad = Math.toRadians(mAnom)
        val c = (1.914602 - t * (0.004817 + t * 0.000014)) * sin(mRad) +
            (0.019993 - t * 0.000101) * sin(2 * mRad) +
            0.000289 * sin(3 * mRad)
        val trueLong = l0 + c
        val omega = 125.04 - 1934.136 * t
        val lambda = trueLong - 0.00569 - 0.00478 * sin(Math.toRadians(omega))
        val epsilon0 = 23.0 + (26.0 + (21.448 - t * (46.815 + t * (0.00059 - t * 0.001813))) / 60.0) / 60.0
        val epsilon = epsilon0 + 0.00256 * cos(Math.toRadians(omega))
        val lambdaRad = Math.toRadians(lambda)
        val epsRad = Math.toRadians(epsilon)
        val declination = asin(sin(epsRad) * sin(lambdaRad))

        // Equation of time (minutes) -> apparent solar time -> hour angle.
        val yTerm = tan(Math.toRadians(epsilon / 2.0)).let { it * it }
        val l0Rad = Math.toRadians(l0)
        val eqTime = 4.0 * Math.toDegrees(
            yTerm * sin(2 * l0Rad) - 2 * e * sin(mRad) +
                4 * e * yTerm * sin(mRad) * cos(2 * l0Rad) -
                0.5 * yTerm * yTerm * sin(4 * l0Rad) -
                1.25 * e * e * sin(2 * mRad),
        )
        val trueSolarTime = (hour * 60.0 + eqTime + 4.0 * LON) % 1440.0
        var hourAngle = trueSolarTime / 4.0 - 180.0
        if (hourAngle < -180.0) hourAngle += 360.0
        val haRad = Math.toRadians(hourAngle)

        val latRad = Math.toRadians(LAT)
        val zenith = acos(
            sin(latRad) * sin(declination) +
                cos(latRad) * cos(declination) * cos(haRad),
        )
        val trueAltitude = 90.0 - Math.toDegrees(zenith)
        // Atmospheric refraction (NOAA): the atmosphere lifts the apparent sun
        // by ~0.57 deg at the horizon, so sunrise, sunset and every twilight
        // threshold land a couple of minutes off without it. The grading keys
        // on apparent altitude, so this is the number that matters.
        val altitude = trueAltitude + refractionDeg(trueAltitude)

        var azimuth = Math.toDegrees(
            acos(
                ((sin(latRad) * cos(zenith)) - sin(declination)) /
                    (cos(latRad) * sin(zenith)).coerceIn(-1.0, 1.0).let {
                        if (abs(it) < 1e-9) 1e-9 else it
                    },
            ).coerceIn(0.0, Math.PI),
        )
        azimuth = if (hourAngle > 0) norm360(azimuth + 180.0) else norm360(540.0 - azimuth)

        return Sun(altitude, azimuth)
    }

    /** Atmospheric refraction correction in degrees for a true altitude. */
    private fun refractionDeg(h: Double): Double {
        if (h > 85.0) return 0.0
        val te = tan(Math.toRadians(h))
        val arcsec = when {
            h > 5.0 -> 58.1 / te - 0.07 / (te * te * te) +
                0.000086 / (te * te * te * te * te)
            h > -0.575 -> 1735.0 + h * (-518.2 + h * (103.4 + h * (-12.79 + h * 0.711)))
            else -> -20.774 / te
        }
        return arcsec / 3600.0
    }

    private fun norm360(v: Double): Double {
        var r = v % 360.0
        if (r < 0) r += 360.0
        return r
    }
}
