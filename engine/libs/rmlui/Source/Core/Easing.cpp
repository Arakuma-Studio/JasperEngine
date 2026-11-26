#include "Easing.h"
#include "../../Include/RmlUi/Core/Math.h"
#include "../../Include/RmlUi/Core/PropertySpecification.h"
#include "../../Include/RmlUi/Core/StringUtilities.h"
#include <cmath>
#include <stdlib.h>

namespace Rml {

namespace Easing {

	namespace {

		// The solver of WebKit's UnitBezier (Chromium's gfx::CubicBezier): the curve is sampled as polynomials in its
		// parameter, and the parameter for an input progress is found with Newton's method, falling back to bisection.
		struct UnitBezier {
			UnitBezier(double x1, double y1, double x2, double y2)
			{
				cx = 3.0 * x1;
				bx = 3.0 * (x2 - x1) - cx;
				ax = 1.0 - cx - bx;
				cy = 3.0 * y1;
				by = 3.0 * (y2 - y1) - cy;
				ay = 1.0 - cy - by;
			}

			double SampleX(double t) const { return ((ax * t + bx) * t + cx) * t; }
			double SampleY(double t) const { return ((ay * t + by) * t + cy) * t; }
			double SampleDerivativeX(double t) const { return (3.0 * ax * t + 2.0 * bx) * t + cx; }

			double SolveX(double x) const
			{
				constexpr double epsilon = 1e-12;
				double t = x;
				for (int i = 0; i < 8; i++)
				{
					const double error = SampleX(t) - x;
					if (error == 0.0)
						return t;
					const double derivative = SampleDerivativeX(t);
					if (std::fabs(derivative) < 1e-6)
						break;
					const double step = error / derivative;
					if (std::fabs(step) < epsilon)
						return t;
					t -= step;
					if (t < 0.0 || t > 1.0)
						break;
				}

				double lower = 0.0, upper = 1.0;
				t = 0.5;
				// Bound the parameter error: a tiny input error can still give a visibly wrong output near a vertical tangent.
				while (upper - lower > epsilon)
				{
					const double sample = SampleX(t);
					if (sample == x)
						return t;
					if (x > sample)
						lower = t;
					else
						upper = t;
					t = (upper - lower) * 0.5 + lower;
				}
				return t;
			}

			double ax, bx, cx, ay, by, cy;
		};

		bool IsDigit(char c)
		{
			return c >= '0' && c <= '9';
		}

		// A CSS <number> token, without a unit: [+-]? (digits [. digits] | . digits) ([eE] [+-]? digits)?
		bool ParseNumberToken(const String& text, float& result)
		{
			size_t i = 0;
			const size_t n = text.size();
			if (i < n && (text[i] == '+' || text[i] == '-'))
				i++;
			bool digits = false;
			while (i < n && IsDigit(text[i]))
			{
				i++;
				digits = true;
			}
			if (i < n && text[i] == '.')
			{
				i++;
				bool fraction = false;
				while (i < n && IsDigit(text[i]))
				{
					i++;
					fraction = true;
				}
				if (!fraction)
					return false;
				digits = true;
			}
			if (!digits)
				return false;
			if (i < n && (text[i] == 'e' || text[i] == 'E'))
			{
				size_t j = i + 1;
				if (j < n && (text[j] == '+' || text[j] == '-'))
					j++;
				if (j < n && IsDigit(text[j]))
				{
					while (j < n && IsDigit(text[j]))
						j++;
					i = j;
				}
			}
			if (i != n)
				return false;
			result = (float)strtod(text.c_str(), nullptr);
			return std::isfinite(result);
		}

		bool ParsePercentage(const String& text, float& result)
		{
			if (text.size() < 2 || text.back() != '%')
				return false;
			if (!ParseNumberToken(text.substr(0, text.size() - 1), result))
				return false;
			result *= 0.01f;
			return true;
		}

		// Splits the arguments of a function at its commas; false if an argument is empty.
		bool SplitArguments(const String& arguments, StringList& result)
		{
			result.clear();
			SplitCommaList(arguments, result);
			for (const String& argument : result)
			{
				if (argument.empty())
					return false;
			}
			return !result.empty();
		}

		bool ParseCubicBezier(const String& arguments, Tween& tween)
		{
			StringList list;
			if (!SplitArguments(arguments, list) || list.size() != 4)
				return false;
			float p[4];
			for (int i = 0; i < 4; i++)
				if (!ParseNumberToken(list[i], p[i]))
					return false;
			if (p[0] < 0.f || p[0] > 1.f || p[2] < 0.f || p[2] > 1.f)
				return false;
			tween = Tween::CubicBezier(p[0], p[1], p[2], p[3]);
			return true;
		}

		bool ParseSteps(const String& arguments, Tween& tween)
		{
			StringList list;
			if (!SplitArguments(arguments, list) || list.size() > 2)
				return false;
			// An <integer>: digits only, with an optional sign.
			const String& count_text = list[0];
			size_t first = (count_text[0] == '+' ? 1 : 0);
			if (first >= count_text.size())
				return false;
			for (size_t i = first; i < count_text.size(); i++)
				if (!IsDigit(count_text[i]))
					return false;
			const long count = strtol(count_text.c_str() + first, nullptr, 10);
			if (count < 1 || count > 1000000)
				return false;

			Tween::StepPosition position = Tween::StepPosition::JumpEnd;
			if (list.size() == 2)
			{
				const String keyword = StringUtilities::ToLower(list[1]);
				if (keyword == "jump-start" || keyword == "start")
					position = Tween::StepPosition::JumpStart;
				else if (keyword == "jump-end" || keyword == "end")
					position = Tween::StepPosition::JumpEnd;
				else if (keyword == "jump-none")
					position = Tween::StepPosition::JumpNone;
				else if (keyword == "jump-both")
					position = Tween::StepPosition::JumpBoth;
				else
					return false;
			}
			if (position == Tween::StepPosition::JumpNone && count < 2)
				return false;
			tween = Tween::Steps((int)count, position);
			return true;
		}

		// linear( [ <number> && <percentage>{0,2} ]# ), CSS Easing 2: control points with missing inputs filled in.
		bool ParseLinear(const String& arguments, Tween& tween)
		{
			StringList list;
			if (!SplitArguments(arguments, list))
				return false;

			struct Point {
				float output;
				float input;
				bool has_input;
			};
			Vector<Point> points;
			for (const String& argument : list)
			{
				StringList words;
				SplitWhitespaceList(argument, words);
				bool has_output = false;
				float output = 0.f;
				Vector<float> inputs;
				for (const String& word : words)
				{
					if (word.empty())
						continue;
					float value = 0.f;
					if (ParsePercentage(word, value))
					{
						if (inputs.size() == 2)
							return false;
						inputs.push_back(value);
					}
					else if (!has_output && ParseNumberToken(word, value))
					{
						has_output = true;
						output = value;
					}
					else
						return false;
				}
				if (!has_output)
					return false;
				if (inputs.empty())
					points.push_back(Point{output, 0.f, false});
				for (float input : inputs)
					points.push_back(Point{output, input, true});
			}
			// The first and last inputs default to 0% and 100%; an input below an earlier one takes the largest earlier
			// input; runs of points without input are spread evenly between their neighbours.
			if (!points.front().has_input)
			{
				points.front().input = 0.f;
				points.front().has_input = true;
			}
			if (!points.back().has_input)
			{
				points.back().input = 1.f;
				points.back().has_input = true;
			}
			float largest = points.front().input;
			for (Point& point : points)
			{
				if (!point.has_input)
					continue;
				if (point.input < largest)
					point.input = largest;
				largest = point.input;
			}
			for (size_t i = 1; i < points.size(); i++)
			{
				if (points[i].has_input)
					continue;
				size_t j = i;
				while (!points[j].has_input)
					j++;
				const float start = points[i - 1].input;
				const float end = points[j].input;
				for (size_t k = i; k < j; k++)
				{
					points[k].input = start + (end - start) * float(k - i + 1) / float(j - i + 1);
					points[k].has_input = true;
				}
				i = j;
			}

			Vector<Tween::LinearStop> stops;
			stops.reserve(points.size());
			for (const Point& point : points)
				stops.push_back(Tween::LinearStop{point.input, point.output});
			tween = Tween::LinearFunction(std::move(stops));
			return true;
		}

		bool ParseLegacyName(const String& name, Tween& tween)
		{
			static const char* const type_names[] = {"back", "bounce", "circular", "cubic", "elastic", "exponential", "linear", "quadratic",
				"quartic", "quintic", "sine"};
			static const Tween::Type types[] = {Tween::Back, Tween::Bounce, Tween::Circular, Tween::Cubic, Tween::Elastic, Tween::Exponential,
				Tween::Linear, Tween::Quadratic, Tween::Quartic, Tween::Quintic, Tween::Sine};

			const size_t dash = name.find('-');
			if (dash == String::npos)
				return false;
			const String type_name = name.substr(0, dash);
			const String direction_name = name.substr(dash + 1);
			Tween::Direction direction;
			if (direction_name == "in")
				direction = Tween::In;
			else if (direction_name == "out")
				direction = Tween::Out;
			else if (direction_name == "in-out")
				direction = Tween::InOut;
			else
				return false;
			for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
			{
				if (type_name == type_names[i])
				{
					tween = Tween(types[i], direction);
					return true;
				}
			}
			return false;
		}

	} // namespace

	float EvaluateCubicBezier(float x1, float y1, float x2, float y2, float x)
	{
		if (x < 0.f)
		{
			float gradient = 0.f;
			if (x1 > 0.f)
				gradient = y1 / x1;
			else if (x2 > 0.f)
				gradient = y2 / x2;
			return gradient * x;
		}
		if (x > 1.f)
		{
			float gradient = 0.f;
			if (x2 < 1.f)
				gradient = (y2 - 1.f) / (x2 - 1.f);
			else if (x1 < 1.f)
				gradient = (y1 - 1.f) / (x1 - 1.f);
			return 1.f + gradient * (x - 1.f);
		}
		if (x1 == y1 && x2 == y2)
			return x;

		const UnitBezier bezier(x1, y1, x2, y2);
		return (float)bezier.SampleY(bezier.SolveX(x));
	}

	float EvaluateSteps(int count, Tween::StepPosition position, float t, bool before_flag)
	{
		using Position = Tween::StepPosition;
		const double scaled = double(t) * double(count);
		double current_step = std::floor(scaled);
		if (position == Position::JumpStart || position == Position::JumpBoth)
			current_step += 1.0;
		if (before_flag && std::fmod(scaled, 1.0) == 0.0)
			current_step -= 1.0;
		if (t >= 0.f && current_step < 0.0)
			current_step = 0.0;

		int jumps = count;
		if (position == Position::JumpBoth)
			jumps = count + 1;
		else if (position == Position::JumpNone)
			jumps = count - 1;
		if (jumps < 1)
			return t;

		if (t <= 1.f && current_step > double(jumps))
			current_step = double(jumps);
		return float(current_step / double(jumps));
	}

	float EvaluateLinear(const Vector<Tween::LinearStop>& stops, float t, bool before_flag)
	{
		if (stops.empty())
			return t;
		if (stops.size() == 1 || (before_flag && t == stops.front().input))
			return stops.front().output;
		if (t < stops.front().input && stops[0].input == stops[1].input)
			return stops.front().output;

		// The last point whose input is at most t (the first one if there is none), and the point after it; the last
		// segment is extended past the end.
		size_t a = 0;
		for (size_t i = 0; i < stops.size(); i++)
		{
			if (stops[i].input <= t)
				a = i;
		}
		if (t == stops[a].input)
			return stops[a].output;
		if (a == stops.size() - 1)
			a--;
		const Tween::LinearStop& p0 = stops[a];
		const Tween::LinearStop& p1 = stops[a + 1];
		if (p0.input == p1.input)
			return p1.output;
		const float progress = (t - p0.input) / (p1.input - p0.input);
		return p0.output + progress * (p1.output - p0.output);
	}

	bool Parse(const String& value, Tween& tween)
	{
		const String text = StringUtilities::StripWhitespace(value);
		if (text.empty())
			return false;

		const size_t open = text.find('(');
		if (open == String::npos)
		{
			const String keyword = StringUtilities::ToLower(text);
			if (keyword == "linear")
				tween = Tween::LinearFunction({});
			else if (keyword == "ease")
				tween = Ease();
			else if (keyword == "ease-in")
				tween = Tween::CubicBezier(0.42f, 0.f, 1.f, 1.f);
			else if (keyword == "ease-out")
				tween = Tween::CubicBezier(0.f, 0.f, 0.58f, 1.f);
			else if (keyword == "ease-in-out")
				tween = Tween::CubicBezier(0.42f, 0.f, 0.58f, 1.f);
			else if (keyword == "step-start")
				tween = Tween::Steps(1, Tween::StepPosition::JumpStart);
			else if (keyword == "step-end")
				tween = Tween::Steps(1, Tween::StepPosition::JumpEnd);
			else
				return ParseLegacyName(keyword, tween);
			return true;
		}

		if (text.back() != ')')
			return false;
		const String name = StringUtilities::ToLower(StringUtilities::StripWhitespace(text.substr(0, open)));
		const String arguments = text.substr(open + 1, text.size() - open - 2);
		if (arguments.find_first_of("()") != String::npos)
			return false;
		if (name == "cubic-bezier")
			return ParseCubicBezier(arguments, tween);
		if (name == "steps")
			return ParseSteps(arguments, tween);
		if (name == "linear")
			return ParseLinear(arguments, tween);
		return false;
	}

	bool ParseNumber(const String& text, float& result)
	{
		return ParseNumberToken(text, result);
	}

	Tween Ease()
	{
		return Tween::CubicBezier(0.25f, 0.1f, 0.25f, 1.f);
	}

} // namespace Easing

} // namespace Rml
