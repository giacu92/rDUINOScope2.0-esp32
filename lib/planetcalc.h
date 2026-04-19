// =============================================================================
// PlanetCalc.h
// Libreria header-only per il calcolo delle posizioni planetarie
// Metodo di Paul Schlyter - https://stjarnhimlen.se/comp/ppcomp.html
//
// Accuratezza: ~1-2 arcominuti (Sole/Luna/pianeti interni),
//              ~1-2 arcominuti (pianeti esterni)
//
// Compatibile con ESP32 / PlatformIO (no heap dinamico, no std::string).
//
// =============================================================================

#pragma once

#include <math.h>
#include <stdint.h>

// -----------------------------------------------------------------------------
// Indici oggetti (da passare a PlanetCalc::compute())
// -----------------------------------------------------------------------------
enum PlanetID : uint8_t {
    PLANET_SUN     = 0,
    PLANET_MERCURY = 1,
    PLANET_VENUS   = 2,
    PLANET_MARS    = 3,
    PLANET_MOON    = 4,
    PLANET_JUPITER = 5,
    PLANET_SATURN  = 6,
    PLANET_URANUS  = 7,
    PLANET_NEPTUNE = 8,
    PLANET_PLUTO   = 9
};

// -----------------------------------------------------------------------------
// Struttura risultato restituita da PlanetCalc::compute()
// Tutti gli angoli sono in gradi decimali.
// -----------------------------------------------------------------------------
struct PlanetData {
    uint8_t  id;           // PlanetID dell'oggetto calcolato
    float    ra_deg;       // Ascensione Retta [0, 360) gradi
    float    dec_deg;      // Declinazione [-90, +90] gradi
    float    dist;         // Distanza geocentrica: AU per i pianeti, km per la Luna
    float    elong_deg;    // Elongazione dal Sole [gradi]
    float    phase;        // Fase illuminata [0.0 .. 1.0]
    float    mag;          // Magnitudine apparente (0 per Plutone)
    float    lon_ecl;      // Longitudine eclittica geocentrica [gradi]
    float    lat_ecl;      // Latitudine eclittica geocentrica [gradi]
    bool     valid;        // true se il calcolo è andato a buon fine
};

// =============================================================================
// Classe PlanetCalc
// Uso:
//   PlanetCalc pc;
//   PlanetData d = pc.compute(PLANET_MARS, 2026, 4, 19, 21, 0);
// =============================================================================
class PlanetCalc {

public:
    // -------------------------------------------------------------------------
    // compute()
    // Calcola la posizione del corpo celeste identificato da 'planet'.
    // Data/ora in UT (Tempo Universale).
    //   year  : anno  (es. 2026)
    //   month : mese  [1..12]
    //   day   : giorno[1..31]
    //   hour  : ore   [0..23]
    //   minute: minuti[0..59]
    // Restituisce un PlanetData con valid=true se l'id è nel range [0..9].
    // -------------------------------------------------------------------------
    PlanetData compute(uint8_t planet,
                       int year, int month, int day,
                       int hour, int minute) const
    {
        PlanetData res;
        res.id    = planet;
        res.valid = false;

        if (planet > 9) return res;

        double UT = hour + minute / 60.0;
        double d  = _day_number(year, month, day, UT);

        // Posizione del Sole (sempre necessaria)
        double ra_sun, dec_sun, rs, lonsun, ecl;
        _sun_position(d, ra_sun, dec_sun, rs, lonsun, ecl);

        if (planet == PLANET_SUN)
        {
            res.ra_deg    = (float)ra_sun;
            res.dec_deg   = (float)dec_sun;
            res.dist      = (float)rs;          // AU
            res.elong_deg = 0.0f;
            res.phase     = 1.0f;
            res.mag       = -26.74f;
            res.lon_ecl   = (float)lonsun;
            res.lat_ecl   = 0.0f;
            res.valid     = true;
        }
        else if (planet == PLANET_MOON)
        {
            _moon_result(d, rs, lonsun, ecl, res);
            res.valid = true;
        }
        else if (planet == PLANET_PLUTO)
        {
            _pluto_result(d, rs, lonsun, ecl, res);
            res.valid = true;
        }
        else
        {
            // mappa indice utente -> indice interno (0..6)
            // 1=Merc, 2=Ven, 3=Marte, 5=Giove, 6=Sat, 7=Urano, 8=Nett
            static const int8_t pmap[10] = { -1, 0, 1, 2, -1, 3, 4, 5, 6, -1 };
            int pno = pmap[planet];
            if (pno < 0) return res;
            _planet_result(pno, d, rs, lonsun, ecl, res);
            res.valid = true;
        }

        return res;
    }

    // -------------------------------------------------------------------------
    // Restituisce il nome testuale dell'oggetto (stringa costante, no heap)
    // -------------------------------------------------------------------------
    static const char* name(uint8_t planet)
    {
        static const char* const names[10] = {
            "SOLE", "MERCURIO", "VENERE", "MARTE", "LUNA",
            "GIOVE", "SATURNO", "URANO", "NETTUNO", "PLUTONE"
        };
        return (planet < 10) ? names[planet] : "SCONOSCIUTO";
    }

// =============================================================================
private:
// =============================================================================

    static constexpr double _PI  = 3.14159265358979323846;
    static constexpr double _RAD = _PI / 180.0;
    static constexpr double _DEG = 180.0 / _PI;

    // -------------------------------------------------------------------------
    static double _rev(double x)
    {
        x = fmod(x, 360.0);
        if (x < 0.0) x += 360.0;
        return x;
    }

    // -------------------------------------------------------------------------
    // Day number di Schlyter (d=0 -> J2000.0)
    // -------------------------------------------------------------------------
    static double _day_number(int y, int m, int D, double UT)
    {
        double d = 367.0 * y
                 - (int)(7 * (y + (m + 9) / 12) / 4)
                 - (int)(3 * ((y + (m - 9) / 7) / 100 + 1) / 4)
                 + (int)(275 * m / 9)
                 + D - 730515;
        d += UT / 24.0;
        return d;
    }

    // -------------------------------------------------------------------------
    // Equazione di Keplero (Newton, M in gradi -> E in gradi)
    // -------------------------------------------------------------------------
    static double _kepler(double M_deg, double e)
    {
        double M = _rev(M_deg) * _RAD;
        double E = M + e * sin(M) * (1.0 + e * cos(M));
        for (int i = 0; i < 50; ++i) {
            double dE = (E - e * sin(E) - M) / (1.0 - e * cos(E));
            E -= dE;
            if (fabs(dE) < 1e-12) break;
        }
        return E * _DEG;
    }

    // -------------------------------------------------------------------------
    static void _eccentric_to_vr(double E_deg, double e, double a,
                                  double& v_deg, double& r)
    {
        double E  = E_deg * _RAD;
        double xv = a * (cos(E) - e);
        double yv = a * (sqrt(1.0 - e * e) * sin(E));
        v_deg = _rev(atan2(yv, xv) * _DEG);
        r     = sqrt(xv * xv + yv * yv);
    }

    // -------------------------------------------------------------------------
    static void _helio_rect(double v_deg, double r,
                             double N_deg, double w_deg, double i_deg,
                             double& xh, double& yh, double& zh)
    {
        double vw = (v_deg + w_deg) * _RAD;
        double N  = N_deg * _RAD;
        double ic = cos(i_deg * _RAD);
        double is = sin(i_deg * _RAD);
        xh = r * (cos(N) * cos(vw) - sin(N) * sin(vw) * ic);
        yh = r * (sin(N) * cos(vw) + cos(N) * sin(vw) * ic);
        zh = r * (sin(vw) * is);
    }

    // -------------------------------------------------------------------------
    // Posizione del Sole
    // -------------------------------------------------------------------------
    static void _sun_position(double d,
                               double& RA, double& Dec,
                               double& rs, double& lonsun, double& ecl)
    {
        double w = _rev(282.9404 + 4.70935e-5  * d);
        double e = 0.016709   - 1.151e-9   * d;
        double M = _rev(356.0470 + 0.9856002585 * d);

        double E = _kepler(M, e);
        double v, r;
        _eccentric_to_vr(E, e, 1.0, v, r);
        rs = r;

        lonsun = _rev(v + w);
        ecl    = 23.4393 - 3.563e-7 * d;

        double xs = r * cos(lonsun * _RAD);
        double ys = r * sin(lonsun * _RAD);
        double xe = xs;
        double ye = ys * cos(ecl * _RAD);
        double ze = ys * sin(ecl * _RAD);

        RA  = _rev(atan2(ye, xe) * _DEG);
        Dec = atan2(ze, sqrt(xe * xe + ye * ye)) * _DEG;
    }

    // -------------------------------------------------------------------------
    // Luna
    // -------------------------------------------------------------------------
    static void _moon_result(double d, double rs, double lonsun, double ecl,
                              PlanetData& res)
    {
        double N  = _rev(125.1228 - 0.0529538083  * d);
        double i  = 5.1454;
        double w  = _rev(318.0634 + 0.1643573223  * d);
        double a  = 60.2666;
        double e  = 0.054900;
        double M  = _rev(115.3654 + 13.0649929509 * d);

        double E = _kepler(M, e);
        double v, r;
        _eccentric_to_vr(E, e, a, v, r);

        double xh, yh, zh;
        _helio_rect(v, r, N, w, i, xh, yh, zh);

        double lonecl = _rev(atan2(yh, xh) * _DEG);
        double latecl = atan2(zh, sqrt(xh * xh + yh * yh)) * _DEG;

        // Perturbazioni
        double Ms = _rev(356.0470 + 0.9856002585 * d);
        double Mm = M;
        double Nm = N;
        double ws = _rev(282.9404 + 4.70935e-5   * d);
        double wm = w;
        double Ls = _rev(Ms + ws);
        double Lm = _rev(Mm + wm + Nm);
        double D  = _rev(Lm - Ls);
        double F  = _rev(Lm - Nm);

        double dlon = 0.0, dlat = 0.0, dr = 0.0;

        dlon += -1.274 * sin((Mm - 2*D)       * _RAD);
        dlon += +0.658 * sin((2*D)             * _RAD);
        dlon += -0.186 * sin(Ms                * _RAD);
        dlon += -0.059 * sin((2*Mm - 2*D)      * _RAD);
        dlon += -0.057 * sin((Mm - 2*D + Ms)   * _RAD);
        dlon += +0.053 * sin((Mm + 2*D)        * _RAD);
        dlon += +0.046 * sin((2*D - Ms)        * _RAD);
        dlon += +0.041 * sin((Mm - Ms)         * _RAD);
        dlon += -0.035 * sin(D                 * _RAD);
        dlon += -0.031 * sin((Mm + Ms)         * _RAD);
        dlon += -0.015 * sin((2*F - 2*D)       * _RAD);
        dlon += +0.011 * sin((Mm - 4*D)        * _RAD);

        dlat += -0.173 * sin((F - 2*D)         * _RAD);
        dlat += -0.055 * sin((Mm - F - 2*D)    * _RAD);
        dlat += -0.046 * sin((Mm + F - 2*D)    * _RAD);
        dlat += +0.033 * sin((F + 2*D)         * _RAD);
        dlat += +0.017 * sin((2*Mm + F)        * _RAD);

        dr += -0.58 * cos((Mm - 2*D) * _RAD);
        dr += -0.46 * cos((2*D)      * _RAD);

        lonecl = _rev(lonecl + dlon);
        latecl += dlat;
        r      += dr;

        // Coordinate geocentriche rettangolari eclittiche
        double xg = r * cos(latecl * _RAD) * cos(lonecl * _RAD);
        double yg = r * cos(latecl * _RAD) * sin(lonecl * _RAD);
        double zg = r * sin(latecl * _RAD);

        double ec = ecl * _RAD;
        double xe = xg;
        double ye = yg * cos(ec) - zg * sin(ec);
        double ze = yg * sin(ec) + zg * cos(ec);

        double RA_d  = _rev(atan2(ye, xe) * _DEG);
        double Dec_d = atan2(ze, sqrt(xe * xe + ye * ye)) * _DEG;

        double elong = acos(cos((lonsun - lonecl) * _RAD) * cos(latecl * _RAD)) * _DEG;
        double FV    = 180.0 - elong;

        res.ra_deg    = (float)RA_d;
        res.dec_deg   = (float)Dec_d;
        res.dist      = (float)(r * 6378.0);  // raggi terrestri -> km
        res.elong_deg = (float)elong;
        res.phase     = (float)((1.0 + cos(FV * _RAD)) / 2.0);
        res.mag       = (float)(-21.62 + 5.0*log10(rs * r)
                        + 0.026 * fabs(FV)
                        + 4.0e-9 * pow(fabs(FV), 4.0));
        res.lon_ecl   = (float)lonecl;
        res.lat_ecl   = (float)latecl;
    }

    // -------------------------------------------------------------------------
    // Pianeti (Mercurio..Nettuno), pno: 0=Merc,1=Ven,2=Mars,3=Jup,4=Sat,5=Ura,6=Nep
    // -------------------------------------------------------------------------
    static void _planet_result(int pno, double d,
                                double rs, double lonsun, double ecl,
                                PlanetData& res)
    {
        struct OrbElem { double N, i, w, a, e, M; };
        OrbElem pl = {};

        switch (pno)
        {
        case 0: // Mercurio
            pl.N = _rev( 48.3313 + 3.24587e-5  * d);
            pl.i =        7.0047 + 5.00e-8     * d;
            pl.w = _rev( 29.1241 + 1.01444e-5  * d);
            pl.a = 0.387098;
            pl.e = 0.205635 + 5.59e-10 * d;
            pl.M = _rev(168.6562 + 4.0923344368 * d);
            break;
        case 1: // Venere
            pl.N = _rev( 76.6799 + 2.46590e-5  * d);
            pl.i =        3.3946 + 2.75e-8     * d;
            pl.w = _rev( 54.8910 + 1.38374e-5  * d);
            pl.a = 0.723330;
            pl.e = 0.006773 - 1.302e-9 * d;
            pl.M = _rev( 48.0052 + 1.6021302244 * d);
            break;
        case 2: // Marte
            pl.N = _rev( 49.5574 + 2.11081e-5  * d);
            pl.i =        1.8497 - 1.78e-8     * d;
            pl.w = _rev(286.5016 + 2.92961e-5  * d);
            pl.a = 1.523688;
            pl.e = 0.093405 + 2.516e-9 * d;
            pl.M = _rev( 18.6021 + 0.5240207766 * d);
            break;
        case 3: // Giove
            pl.N = _rev(100.4542 + 2.76854e-5  * d);
            pl.i =        1.3030 - 1.557e-7    * d;
            pl.w = _rev(273.8777 + 1.64505e-5  * d);
            pl.a = 5.20256;
            pl.e = 0.048498 + 4.469e-9 * d;
            pl.M = _rev( 19.8950 + 0.0830853001 * d);
            break;
        case 4: // Saturno
            pl.N = _rev(113.6634 + 2.38980e-5  * d);
            pl.i =        2.4886 - 1.081e-7    * d;
            pl.w = _rev(339.3939 + 2.97661e-5  * d);
            pl.a = 9.55475;
            pl.e = 0.055546 - 9.499e-9 * d;
            pl.M = _rev(316.9670 + 0.0334442282 * d);
            break;
        case 5: // Urano
            pl.N = _rev( 74.0005 + 1.3978e-5   * d);
            pl.i =        0.7733 + 1.9e-8      * d;
            pl.w = _rev( 96.6612 + 3.0565e-5   * d);
            pl.a = 19.18171 - 1.55e-8 * d;
            pl.e = 0.047318 + 7.45e-9 * d;
            pl.M = _rev(142.5905 + 0.011725806  * d);
            break;
        case 6: // Nettuno
            pl.N = _rev(131.7806 + 3.0173e-5   * d);
            pl.i =        1.7700 - 2.55e-7     * d;
            pl.w = _rev(272.8461 - 6.027e-6    * d);
            pl.a = 30.05826 + 3.313e-8 * d;
            pl.e = 0.008606 + 2.15e-9  * d;
            pl.M = _rev(260.2471 + 0.005995147  * d);
            break;
        default: return;
        }

        double E = _kepler(pl.M, pl.e);
        double v, r;
        _eccentric_to_vr(E, pl.e, pl.a, v, r);

        double xh, yh, zh;
        _helio_rect(v, r, pl.N, pl.w, pl.i, xh, yh, zh);

        double lonecl = _rev(atan2(yh, xh) * _DEG);
        double latecl = atan2(zh, sqrt(xh * xh + yh * yh)) * _DEG;

        // Perturbazioni
        double Mj = (pno >= 3) ? _rev( 19.8950 + 0.0830853001 * d) : 0.0;
        double Ms = (pno >= 4) ? _rev(316.9670 + 0.0334442282 * d) : 0.0;
        double Mu = (pno >= 5) ? _rev(142.5905 + 0.011725806  * d) : 0.0;

        double dlon = 0.0, dlat = 0.0;

        if (pno == 3) { // Giove
            dlon += -0.332 * sin((2*Mj - 5*Ms - 67.6) * _RAD);
            dlon += -0.056 * sin((2*Mj - 2*Ms + 21.0) * _RAD);
            dlon += +0.042 * sin((3*Mj - 5*Ms + 21.0) * _RAD);
            dlon += -0.036 * sin((  Mj - 2*Ms)         * _RAD);
            dlon += +0.022 * cos((  Mj -   Ms)         * _RAD);
            dlon += +0.023 * sin((2*Mj - 3*Ms + 52.0) * _RAD);
            dlon += -0.016 * sin((  Mj - 5*Ms - 69.0) * _RAD);
        }
        else if (pno == 4) { // Saturno
            dlon += +0.812 * sin((2*Mj - 5*Ms - 67.6) * _RAD);
            dlon += -0.229 * cos((2*Mj - 4*Ms -  2.0) * _RAD);
            dlon += +0.119 * sin((  Mj - 2*Ms -  3.0) * _RAD);
            dlon += +0.046 * sin((2*Mj - 6*Ms - 69.0) * _RAD);
            dlon += +0.014 * sin((  Mj - 3*Ms + 32.0) * _RAD);
            dlat += -0.020 * cos((2*Mj - 4*Ms -  2.0) * _RAD);
            dlat += +0.018 * sin((2*Mj - 6*Ms - 49.0) * _RAD);
        }
        else if (pno == 5) { // Urano
            dlon += +0.040 * sin((  Ms - 2*Mu +  6.0) * _RAD);
            dlon += +0.035 * sin((  Ms - 3*Mu + 33.0) * _RAD);
            dlon += -0.015 * sin((  Mj -   Mu + 20.0) * _RAD);
        }

        lonecl = _rev(lonecl + dlon);
        latecl += dlat;

        xh = r * cos(latecl * _RAD) * cos(lonecl * _RAD);
        yh = r * cos(latecl * _RAD) * sin(lonecl * _RAD);
        zh = r * sin(latecl * _RAD);

        double xs = rs * cos(lonsun * _RAD);
        double ys = rs * sin(lonsun * _RAD);

        double xg = xh + xs;
        double yg = yh + ys;
        double zg = zh;

        double ec = ecl * _RAD;
        double xe = xg;
        double ye = yg * cos(ec) - zg * sin(ec);
        double ze = yg * sin(ec) + zg * cos(ec);

        double RA_d  = _rev(atan2(ye, xe) * _DEG);
        double Dec_d = atan2(ze, sqrt(xe * xe + ye * ye)) * _DEG;
        double rg    = sqrt(xg*xg + yg*yg + zg*zg);

        double elong = acos((rs*rs + rg*rg - r*r) / (2.0 * rs * rg)) * _DEG;
        double FV    = acos((r*r  + rg*rg - rs*rs) / (2.0 * r * rg)) * _DEG;
        double phase = (1.0 + cos(FV * _RAD)) / 2.0;

        double mag = 0.0;
        switch (pno) {
        case 0: mag = -0.36 + 5.0*log10(r*rg) + 0.027*FV + 2.2e-13*pow(FV,6.0); break;
        case 1: mag = -4.34 + 5.0*log10(r*rg) + 0.013*FV + 4.2e-7 *pow(FV,3.0); break;
        case 2: mag = -1.51 + 5.0*log10(r*rg) + 0.016*FV; break;
        case 3: mag = -9.25 + 5.0*log10(r*rg) + 0.014*FV; break;
        case 4: { // Saturno: correzione anelli
            double los = _rev(atan2(yg, xg) * _DEG);
            double las = atan2(zg, sqrt(xg*xg + yg*yg)) * _DEG;
            double ir  = 28.06;
            double Nr  = _rev(169.51 + 3.82e-5 * d);  // d catturato per closure
            double B   = asin(sin(las*_RAD)*cos(ir*_RAD)
                       - cos(las*_RAD)*sin(ir*_RAD)*sin((los-Nr)*_RAD)) * _DEG;
            double ring_magn = -2.6*sin(fabs(B)*_RAD) + 1.2*pow(sin(B*_RAD),2.0);
            mag = -9.0 + 5.0*log10(r*rg) + 0.044*FV + ring_magn;
            break;
        }
        case 5: mag = -7.15 + 5.0*log10(r*rg) + 0.001*FV; break;
        case 6: mag = -6.90 + 5.0*log10(r*rg) + 0.001*FV; break;
        }

        res.ra_deg    = (float)RA_d;
        res.dec_deg   = (float)Dec_d;
        res.dist      = (float)rg;   // AU
        res.elong_deg = (float)elong;
        res.phase     = (float)phase;
        res.mag       = (float)mag;
        res.lon_ecl   = (float)lonecl;
        res.lat_ecl   = (float)latecl;
    }

    // -------------------------------------------------------------------------
    // Plutone (fit numerico, valido ~1800-2100)
    // -------------------------------------------------------------------------
    static void _pluto_result(double d, double rs, double lonsun, double ecl,
                               PlanetData& res)
    {
        double S = _rev( 50.03  + 0.033459652 * d);
        double P = _rev(238.95  + 0.003968789 * d);

        double lonecl =
            238.9508 + 0.00400703 * d
            - 19.799*sin(P*_RAD)   + 19.848*cos(P*_RAD)
            +  0.897*sin(2*P*_RAD) -  4.956*cos(2*P*_RAD)
            +  0.610*sin(3*P*_RAD) +  1.211*cos(3*P*_RAD)
            -  0.341*sin(4*P*_RAD) -  0.190*cos(4*P*_RAD)
            +  0.128*sin(5*P*_RAD) -  0.034*cos(5*P*_RAD)
            -  0.038*sin(6*P*_RAD) +  0.031*cos(6*P*_RAD)
            +  0.020*sin((S-P)*_RAD) - 0.010*cos((S-P)*_RAD);
        lonecl = _rev(lonecl);

        double latecl =
            -3.9082
            -  5.453*sin(P*_RAD)   - 14.975*cos(P*_RAD)
            +  3.527*sin(2*P*_RAD) +  1.673*cos(2*P*_RAD)
            -  1.051*sin(3*P*_RAD) +  0.328*cos(3*P*_RAD)
            +  0.179*sin(4*P*_RAD) -  0.292*cos(4*P*_RAD)
            +  0.019*sin(5*P*_RAD) +  0.100*cos(5*P*_RAD)
            -  0.031*sin(6*P*_RAD) -  0.026*cos(6*P*_RAD)
                                    +  0.011*cos((S-P)*_RAD);

        double r =
            40.72
            + 6.68*sin(P*_RAD)   + 6.90*cos(P*_RAD)
            - 1.18*sin(2*P*_RAD) - 0.03*cos(2*P*_RAD)
            + 0.15*sin(3*P*_RAD) - 0.14*cos(3*P*_RAD);

        double xh = r * cos(latecl*_RAD) * cos(lonecl*_RAD);
        double yh = r * cos(latecl*_RAD) * sin(lonecl*_RAD);
        double zh = r * sin(latecl*_RAD);

        double xs = rs * cos(lonsun*_RAD);
        double ys = rs * sin(lonsun*_RAD);

        double xg = xh + xs;
        double yg = yh + ys;
        double zg = zh;

        double ec = ecl * _RAD;
        double xe = xg;
        double ye = yg * cos(ec) - zg * sin(ec);
        double ze = yg * sin(ec) + zg * cos(ec);

        double RA_d  = _rev(atan2(ye, xe) * _DEG);
        double Dec_d = atan2(ze, sqrt(xe * xe + ye * ye)) * _DEG;
        double rg    = sqrt(xg*xg + yg*yg + zg*zg);

        double elong = acos((rs*rs + rg*rg - r*r) / (2.0 * rs * rg)) * _DEG;
        double FV    = acos((r*r  + rg*rg - rs*rs) / (2.0 * r * rg)) * _DEG;

        res.ra_deg    = (float)RA_d;
        res.dec_deg   = (float)Dec_d;
        res.dist      = (float)rg;    // AU
        res.elong_deg = (float)elong;
        res.phase     = (float)((1.0 + cos(FV * _RAD)) / 2.0);
        res.mag       = 0.0f;         // non disponibile in Schlyter per Plutone
        res.lon_ecl   = (float)lonecl;
        res.lat_ecl   = (float)latecl;
    }
};
