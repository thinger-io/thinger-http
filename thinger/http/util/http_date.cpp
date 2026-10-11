#include "http_date.hpp"
#include <array>
#include <chrono>
#include <format>

namespace thinger::http::util {

namespace {

    constexpr std::array<std::string_view, 7> day_names = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    constexpr std::array<std::string_view, 7> long_day_names = {"Sunday", "Monday", "Tuesday", "Wednesday",
                                                                "Thursday", "Friday", "Saturday"};
    constexpr std::array<std::string_view, 12> month_names = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    struct date_fields {
        int year = 0;
        int month = 0;      // 1-12
        int day = 0;
        int hour = 0;
        int minute = 0;
        int second = 0;
    };

    // Reads the fields of a date from left to right (case-sensitive, as the grammar)
    class date_reader {
    public:
        explicit date_reader(std::string_view text) : text_(text) {}

        bool literal(std::string_view expected) {
            if (!text_.starts_with(expected)) return false;
            text_.remove_prefix(expected.size());
            return true;
        }

        // One of `names`: its index
        template<size_t N>
        bool name(const std::array<std::string_view, N>& names, int& index) {
            for (size_t i = 0; i < N; ++i) {
                if (literal(names[i])) {
                    index = static_cast<int>(i);
                    return true;
                }
            }
            return false;
        }

        // A number of exactly `digits` digits
        bool number(size_t digits, int& value) {
            if (text_.size() < digits) return false;
            value = 0;
            for (size_t i = 0; i < digits; ++i) {
                char c = text_[i];
                if (c < '0' || c > '9') return false;
                value = value * 10 + (c - '0');
            }
            text_.remove_prefix(digits);
            return true;
        }

        // "HH:MM:SS"
        bool time_of_day(date_fields& fields) {
            return number(2, fields.hour) && literal(":") && number(2, fields.minute) && literal(":") &&
                   number(2, fields.second);
        }

        bool done() const { return text_.empty(); }

    private:
        std::string_view text_;
    };

    // IMF-fixdate: "Sun, 06 Nov 1994 08:49:37 GMT"
    bool read_imf_fixdate(std::string_view text, date_fields& fields) {
        date_reader reader(text);
        int weekday = 0;
        if (!(reader.name(day_names, weekday) && reader.literal(", ") && reader.number(2, fields.day) &&
              reader.literal(" ") && reader.name(month_names, fields.month) && reader.literal(" ") &&
              reader.number(4, fields.year) && reader.literal(" ") && reader.time_of_day(fields) &&
              reader.literal(" GMT") && reader.done())) {
            return false;
        }
        fields.month++;
        return true;
    }

    // RFC 850: "Sunday, 06-Nov-94 08:49:37 GMT". The year of two digits is the latest one
    // ending in them that is not more than 50 years in the future (RFC 9110, 5.6.7).
    bool read_rfc850_date(std::string_view text, date_fields& fields) {
        date_reader reader(text);
        int weekday = 0;
        if (!(reader.name(long_day_names, weekday) && reader.literal(", ") && reader.number(2, fields.day) &&
              reader.literal("-") && reader.name(month_names, fields.month) && reader.literal("-") &&
              reader.number(2, fields.year) && reader.literal(" ") && reader.time_of_day(fields) &&
              reader.literal(" GMT") && reader.done())) {
            return false;
        }
        fields.month++;
        using namespace std::chrono;
        int current_year = static_cast<int>(year_month_day{floor<days>(system_clock::now())}.year());
        fields.year += current_year / 100 * 100;
        if (fields.year > current_year + 50) fields.year -= 100;
        return true;
    }

    // asctime: "Sun Nov  6 08:49:37 1994" (a day of one digit has a space before)
    bool read_asctime_date(std::string_view text, date_fields& fields) {
        date_reader reader(text);
        int weekday = 0;
        if (!(reader.name(day_names, weekday) && reader.literal(" ") && reader.name(month_names, fields.month) &&
              reader.literal(" ") && (reader.literal(" ") ? reader.number(1, fields.day) : reader.number(2, fields.day)) &&
              reader.literal(" ") && reader.time_of_day(fields) && reader.literal(" ") &&
              reader.number(4, fields.year) && reader.done())) {
            return false;
        }
        fields.month++;
        return true;
    }

}

std::string format_http_date(std::time_t time) {
    using namespace std::chrono;
    sys_seconds point{seconds{time}};
    auto day_point = floor<days>(point);
    year_month_day date{day_point};
    hh_mm_ss clock{point - day_point};
    return std::format("{}, {:02} {} {:04} {:02}:{:02}:{:02} GMT",
                       day_names[weekday{day_point}.c_encoding()], static_cast<unsigned>(date.day()),
                       month_names[static_cast<unsigned>(date.month()) - 1], static_cast<int>(date.year()),
                       clock.hours().count(), clock.minutes().count(), clock.seconds().count());
}

std::optional<std::time_t> parse_http_date(std::string_view date) {
    date_fields fields;
    if (!read_imf_fixdate(date, fields) && !read_rfc850_date(date, fields) && !read_asctime_date(date, fields)) {
        return std::nullopt;
    }

    using namespace std::chrono;
    year_month_day day_point{year{fields.year}, month{static_cast<unsigned>(fields.month)},
                             day{static_cast<unsigned>(fields.day)}};
    // (a second of 60 is a leap second)
    if (!day_point.ok() || fields.hour > 23 || fields.minute > 59 || fields.second > 60) return std::nullopt;
    auto point = sys_days{day_point} + hours{fields.hour} + minutes{fields.minute} + seconds{fields.second};
    return static_cast<std::time_t>(point.time_since_epoch().count());
}

}
