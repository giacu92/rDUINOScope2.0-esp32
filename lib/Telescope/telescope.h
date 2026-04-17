#include <math.h>
#include <stdio.h>
#include <time.h>
#include <Arduino.h>

enum class State : uint16_t {
    IDLE = 0,
    SLEWING = 1,
    TRACKING = 2,
    ERROR = 3,
    MOTORS_DISABLED = 4,
    MANUAL_JOG = 5
};

enum class TrackingMode : uint16_t {
    LUNAR = 0,
    SIDEREAL = 1,
    SOLAR = 2
};

class Telescope {
public:
    const char *fw_ver = "004 24 2026#";

    State status = State::IDLE;
    uint16_t stm32FirmwareVersion = 0;
    TrackingMode trackingMode = TrackingMode::SIDEREAL;
    bool trackingEnabled = false;
    bool motorsEnabled = true;
    double latitude;    // gradi decimali, Nord positivo
    double longitude;   // gradi decimali, Est positivo
    double ra;          // ore decimali [0,24)
    double dec;         // gradi decimali [-90,+90]
    
    String currentObjectName = "UNKNOWN";

    bool isSlewing = false;

    Telescope(double lat = 0.0, double lon = 0.0)
        : latitude(lat), longitude(lon), ra(6.5), dec(22.0) {
        struct tm initialTime = {};
        initialTime.tm_year = 2026 - 1900;
        initialTime.tm_mon = 0;
        initialTime.tm_mday = 1;
        setLX200DateTime(initialTime);
    }

    // --- Normalizzazioni fondamentali ---
    void normalize() {
        ra = fmod(ra, 24.0);
        if (ra < 0) ra += 24.0;

        if (dec > 90.0) dec = 90.0;
        if (dec < -90.0) dec = -90.0;
    }

    // :GVF# -> "fw_ver"
    void getLX200FwDate(char *out) {
        strcpy(out, fw_ver);
    }

    // :GL# -> "Get Local time"
    void getLX200Localtime(char *out) {
        struct tm t;
        getLX200DateTime(t);
        sprintf(out, "%02d:%02d:%02d#",
                t.tm_hour,
                t.tm_min,
                t.tm_sec);
    }

    void getLX200FwTime(char *out) {
        getLX200Localtime(out);
    }

    bool setLX200Date(const struct tm &t) {
        return setLX200DateTime(t);
    }

    bool getLX200DateTime(struct tm &out) {
        time_t now = currentLX200Time();
        struct tm *local = localtime(&now);
        if (!local) return false;

        out = *local;
        return true;
    }

    bool setLX200DateTime(const struct tm &t) {
        struct tm normalized = t;
        time_t epoch = mktime(&normalized);
        if (epoch == (time_t)-1) return false;

        lx200Time = epoch;
        lx200TimeMillis = millis();
        return true;
    }

    void getLX200Date(char *out) {
        struct tm t;
        getLX200DateTime(t);
        sprintf(out, "%02d/%02d/%02d#",
                t.tm_mon + 1,
                t.tm_mday,
                t.tm_year % 100);
    }

    // :Gt#  -> sDD*MM#
    void getLX200Latitude(char *out) {
        char sign = (latitude >= 0) ? '+' : '-';
        double absLat = fabs(latitude);

        int d = (int)absLat;
        int m = (int)round((absLat - d) * 60.0);
        if (m == 60) { d++; m = 0; }

        sprintf(out, "%c%02d*%02d#", sign, d, m);
    }

    void setLX200Latitude(double _lat) {
        latitude = _lat;
    }


    // :Gg# -> sDDD*MM#  (Meade: Ovest positivo)
    void getLX200Longitude(char *out) {
        double meadeLon = -longitude;

        while (meadeLon < -180) meadeLon += 360;
        while (meadeLon > 180)  meadeLon -= 360;

        char sign = (meadeLon >= 0) ? '+' : '-';
        double absLon = fabs(meadeLon);

        int d = (int)absLon;
        int m = (int)round((absLon - d) * 60.0);
        if (m == 60) { d++; m = 0; }

        sprintf(out, "%c%03d*%02d#", sign, d, m);
    }

    void setLX200Longitude(double _lon) {
        longitude = _lon;
    }

    // :GR# -> HH:MM:SS#
    void getLX200RA(char *out) {
        double totalSeconds = ra * 3600.0;

        int h = (int)(totalSeconds / 3600);
        totalSeconds -= h * 3600;

        int m = (int)(totalSeconds / 60);
        int s = (int)round(totalSeconds - m * 60);

        if (s == 60) { s = 0; m++; }
        if (m == 60) { m = 0; h++; }
        if (h == 24) h = 0;

        sprintf(out, "%02d:%02d:%02d#", h, m, s);
    }

    // :GD# -> sDD*MM'SS#
    void getLX200Dec(char *out) {
        char sign = (dec >= 0) ? '+' : '-';
        double absDec = fabs(dec);

        double totalSeconds = absDec * 3600.0;

        int d = (int)(totalSeconds / 3600);
        totalSeconds -= d * 3600;

        int m = (int)(totalSeconds / 60);
        int s = (int)round(totalSeconds - m * 60);

        if (s == 60) { s = 0; m++; }
        if (m == 60) { m = 0; d++; }

        sprintf(out, "%c%02d*%02d'%02d#", sign, d, m, s);
    }

    void getLX200Sync(char *out) {
        sprintf(out, " %s EX GAL MAG 0.0 SZ0.0'#", currentObjectName.c_str());
    }

    void setSlewing(bool state) {
        isSlewing = state;
    }

private:
    time_t lx200Time = 0;
    unsigned long lx200TimeMillis = 0;

    time_t currentLX200Time() const {
        return lx200Time + (time_t)((millis() - lx200TimeMillis) / 1000UL);
    }

    double julianDay(int Y, int M, int D, int h, int m, int s) {
        if (M <= 2) { Y--; M += 12; }

        int A = Y / 100;
        int B = 2 - A + A / 4;

        double dayFraction = (h + (m + s / 60.0) / 60.0) / 24.0;

        return (int)(365.25 * (Y + 4716))
            + (int)(30.6001 * (M + 1))
            + D + B - 1524.5 + dayFraction;
    }

    double gmst(double JD) {
        double T = (JD - 2451545.0) / 36525.0;

        double GMST =
            280.46061837 +
            360.98564736629 * (JD - 2451545.0) +
            0.000387933 * T * T -
            (T * T * T) / 38710000.0;

        GMST = fmod(GMST, 360.0);
        if (GMST < 0) GMST += 360.0;

        return GMST; // gradi
    }

    double lst(double gmst_deg, double longitude_deg) {
        double LST = gmst_deg + longitude_deg;

        LST = fmod(LST, 360.0);
        if (LST < 0) LST += 360.0;

        return LST;
    }
    
    double lstHours(double lst_deg) {
        return lst_deg / 15.0;
    }

    double getLST_hours(double longitude_deg) {

    struct tm t;
    if (!getLocalTime(&t)) {
        return 0.0; // fallback
    }

    double JD = julianDay(
            t.tm_year + 1900,
            t.tm_mon + 1,
            t.tm_mday,
            t.tm_hour,
            t.tm_min,
            t.tm_sec
        );

        double GMST = gmst(JD);
        double LST_deg = GMST + longitude_deg;

        LST_deg = fmod(LST_deg, 360.0);
        if (LST_deg < 0) LST_deg += 360.0;

        return LST_deg / 15.0;
    }

};
