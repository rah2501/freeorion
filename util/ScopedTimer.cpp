#include "ScopedTimer.h"

#include "Logger.h"

#include <boost/unordered_map.hpp>
#include <boost/chrono.hpp>

#include <iomanip>
#include <sstream>

namespace {
    DeclareThreadSafeLogger(timer);
}

class ScopedTimer::Impl {
public:
    Impl(const std::string& timed_name, bool enable_output, std::chrono::microseconds threshold) :
        m_start(std::chrono::high_resolution_clock::now()),
        m_name(timed_name),
        m_enable_output(enable_output),
        m_threshold(threshold)
    {}

    ~Impl() {
        if (!m_enable_output)
            return;

        std::chrono::nanoseconds duration{std::chrono::high_resolution_clock::now() - m_start};

        if (duration < m_threshold)
            return;

        std::stringstream ss;
        ss << m_name << " time: ";
        FormatDuration(ss, duration);
        DebugLogger(timer) << ss.str();
    }

    void Restart()
    { m_start = std::chrono::high_resolution_clock::now(); }

    template <typename UNITS = std::chrono::seconds>
    double Duration() const {
        using ns = std::chrono::nanoseconds;
        ns duration{std::chrono::high_resolution_clock::now() - m_start};
        return static_cast<double>(duration.count()) / ns::period::den * ns::period::num / UNITS::period::num * UNITS::period::den;
    }

    std::string DurationString() const {
        std::stringstream ss;
        std::chrono::nanoseconds duration{std::chrono::high_resolution_clock::now() - m_start};
        FormatDuration(ss, duration);
        return ss.str();
    }

    static void FormatDuration(std::stringstream& ss, const std::chrono::nanoseconds& duration) {
        ss << std::setw(8) << std::right;
        if (duration >= std::chrono::seconds(20)) {
            ss << std::chrono::duration_cast<std::chrono::seconds>(duration).count() << " s";

        } else if (duration >= std::chrono::seconds(2)) {
            auto ms{std::chrono::duration_cast<std::chrono::milliseconds>(duration).count()};
            ss << (ms / 100) / 10.0 << " s";    // round to 10ths of seconds

        } else if (duration >= std::chrono::milliseconds(20)) {
            ss << std::chrono::duration_cast<std::chrono::milliseconds>(duration).count() << " ms";

        } else if (duration >= std::chrono::milliseconds(2)) {
            auto ms{std::chrono::duration_cast<std::chrono::microseconds>(duration).count()};
            ss << (ms / 100) / 10.0 << " ms";    // round to 10ths of milliseconds

        } else if (duration >= std::chrono::microseconds(20)) {
            ss << std::chrono::duration_cast<std::chrono::microseconds>(duration).count() << " µs";

        } else if (duration >= std::chrono::microseconds(2)) {
            auto ns{std::chrono::duration_cast<std::chrono::microseconds>(duration).count()};
            ss << (ns / 100) / 10.0 << " µs";    // round to 10ths of microseconds

        } else {
            ss << std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count() << " ns";
        }
    }

    std::chrono::high_resolution_clock::time_point m_start;
    std::string                                    m_name;
    bool                                           m_enable_output;
    std::chrono::microseconds                      m_threshold;
};

ScopedTimer::ScopedTimer(const std::string& timed_name, bool enable_output,
                         std::chrono::microseconds threshold) :
    m_impl(new Impl(timed_name, enable_output, threshold))
{}

ScopedTimer::ScopedTimer(const std::string& timed_name,
                         std::chrono::microseconds threshold) :
    m_impl(new Impl(timed_name, true, threshold))
{}

//! @note
//!     ~ScopedTimer is required because Impl is defined here.
ScopedTimer::~ScopedTimer()
{}

void ScopedTimer::restart()
{ m_impl->Restart(); }

double ScopedTimer::duration() const
{ return m_impl->Duration(); }

std::string ScopedTimer::DurationString() const
{ return m_impl->DurationString(); }



class SectionedScopedTimer::Impl : public ScopedTimer::Impl {
    /** Sections store a time and a duration for each section of the elapsed time report.*/
    struct Sections {
        Sections(const std::chrono::high_resolution_clock::time_point& now,
                 const std::chrono::nanoseconds& time_from_start) :
            m_table(),
            m_section_start(now),
            m_curr(),
            m_section_names()
        {
            // Create a dummy "" section so that m_curr is always a valid iterator.
            auto curr = m_table.insert({"", time_from_start});
            m_curr = curr.first;
        }

        /** Add time to the current section and then setup the new section. */
        void Accumulate(const std::chrono::high_resolution_clock::time_point& now,
                        const std::string& section_name)
        {
            if (m_curr->first == section_name)
                return;

            m_curr->second += (now - m_section_start);

            m_section_start = now;

            // Create a new section if needed and update m_curr.
            auto maybe_new = m_table.insert(
                {section_name, std::chrono::high_resolution_clock::duration::zero()});
            m_curr = maybe_new.first;

            // Insert succeed, so grab the new section name.
            if (maybe_new.second)
                m_section_names.push_back(section_name);

        }

        //Table of section durations
        typedef boost::unordered_map<std::string, std::chrono::nanoseconds> SectionTable;
        SectionTable m_table;

        // Currently running section start time and iterator
        std::chrono::high_resolution_clock::time_point m_section_start;

        // m_curr always points to the section currently accumulating
        // time or to the dummy "" blank section.
        SectionTable::iterator m_curr;

        // Names of the sections in order or creation.
        std::vector<std::string> m_section_names;
    };

public:
    Impl(const std::string& timed_name, bool enable_output,
         std::chrono::microseconds threshold) :
        ScopedTimer::Impl(timed_name, enable_output, threshold)
    {}

    /** The destructor will print the table of accumulated times. */
    ~Impl() {
        if (!m_enable_output)
            return;

        std::chrono::nanoseconds duration =
            std::chrono::high_resolution_clock::now() - m_start;

        if (duration > m_threshold)
            return;

        // No table so use basic ScopedTimer output.
        if (!m_sections)
            return;

        //Stop the final section.
        EnterSection("");

        // Don't print the table if the only section is the default section
        auto only_section_is_the_default =
            (m_sections->m_section_names.size() == 1
             && *m_sections->m_section_names.begin() == "");
        if (only_section_is_the_default)
            return;

        //Print the section times followed by the total time elapsed.

        // Find the longest name to right align the times.
        size_t longest_section_name(0);
        for (const std::string& section_name : m_sections->m_section_names)
        { longest_section_name = std::max(longest_section_name, section_name.size()); }


        for (const std::string& section_name : m_sections->m_section_names) {
            auto jt = m_sections->m_table.find(section_name);
            if (jt == m_sections->m_table.end()) {
                ErrorLogger(timer) << "Missing section " << section_name << " in section table.";
                continue;
            }

            // is duration yet long enough to output?
            if (jt->second > m_threshold)
                continue;

            // Create a header with padding, so all times align.
            std::stringstream header, tail;
            FormatDuration(tail, jt->second);
            header << m_name << " - "
                   << std::setw(longest_section_name) << std::left << section_name
                   << std::right << " time: "
                   << tail.str();
            DebugLogger(timer) << header.str();
        }

        // Create a header with padding, so all times align.
        std::stringstream header, tail;
        FormatDuration(tail, duration);
        header << m_name
               << std::setw(longest_section_name + 3 + 7)
               << std::right << " time: "
               << tail.str();
        DebugLogger(timer) << header.str();

        // Prevent the base class from outputting a duplicate total time.
        m_enable_output = false;
    }

    void EnterSection(const std::string& section_name) {
        auto now(std::chrono::high_resolution_clock::now());

        // One time initialization.
        if (!m_sections)
            CreateSections(now);

        m_sections->Accumulate(now, section_name);
    }

    /** CreateSections allow m_sections to only be initialized if it is used.*/
    Sections* CreateSections(const std::chrono::high_resolution_clock::time_point& now) {
        m_sections.reset(new Sections(now, now - m_start));
        return m_sections.get();
    }

    // Pointer to table of sections.
    // Sections are only allocated when the first section is created, to minimize overhead of a
    // section-less timer.
    std::unique_ptr<Sections> m_sections;
};

SectionedScopedTimer::SectionedScopedTimer(const std::string& timed_name,
                                           bool enable_output,
                                           std::chrono::microseconds threshold) :
    m_impl(new Impl(timed_name, enable_output, threshold))
{}

SectionedScopedTimer::SectionedScopedTimer(const std::string& timed_name,
                                           std::chrono::microseconds threshold) :
    m_impl(new Impl(timed_name, true, threshold))
{}

// ~SectionedScopedTimer is required because Impl is defined here.
SectionedScopedTimer::~SectionedScopedTimer()
{}

void SectionedScopedTimer::EnterSection(const std::string& section_name)
{ m_impl->EnterSection(section_name); }
