// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The anime port against reference.json, generated from the real Anime.js 4.5.0 by
// gen_reference.mjs: every easing at 101 points, springs, stagger outputs, timeline offsets
// and positions, value decomposition, colours, utils, and frame-by-frame recordings of
// animations, timers and timelines on plain objects (values, callbacks in order, state).
// CPU only: no GPU, no document.

#include "htmlui/anim/Anime.h"
#include "htmlui/anim/AnimCommon.h"

#include "json.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <map>
#include <sstream>

using namespace jasperengine;
using json = nlohmann::ordered_json;

namespace {

    const json& reference(){
        static const json data = [](){
            std::ifstream file(std::string(JASPER_HTMLUI_TEST_SOURCE_DIR) + "/anime/reference.json");
            json parsed;
            if (file)
                file >> parsed;
            return parsed;
        }();
        return data;
    }

    bool near(double a, double b, double tolerance = 1e-9){
        if (std::isnan(a) && std::isnan(b))
            return true;
        return std::fabs(a - b) <= tolerance * std::max(1.0, std::fabs(b));
    }

    double number(const json& j){
        return j.is_null() ? std::nan("") : j.get<double>();
    }

    // strings compared piecewise: the numbers in them within the tolerance, the rest exactly
    bool sameText(const std::string& a, const std::string& b){
        if (a == b)
            return true;
        std::vector<double> na, nb;
        std::vector<std::string> sa, sb;
        AnimValue::splitComplex(a, na, sa);
        AnimValue::splitComplex(b, nb, sb);
        if (na.size() != nb.size() || sa != sb)
            return false;
        for (size_t i = 0; i < na.size(); i++){
            if (!near(na[i], nb[i]))
                return false;
        }
        return true;
    }

    std::string describe(const AnimRawValue& v){
        return v.isNil() ? "nil" : v.isNumber() ? AnimMath::toString(v.number) : "\"" + v.text + "\"";
    }

    // gen_reference.mjs's markers, mapped to the C++ API
    AnimParam fromJson(const json& j){
        if (j.is_null()) return AnimParam();
        if (j.is_boolean()) return AnimParam(j.get<bool>());
        if (j.is_number()) return AnimParam(j.get<double>());
        if (j.is_string()) return AnimParam(j.get<std::string>());
        if (j.is_array()){
            std::vector<AnimParam> items;
            for (const json& item : j) items.push_back(fromJson(item));
            return AnimParam::array(items);
        }
        if (j.contains("$ease")){
            const json& e = j["$ease"];
            const std::string name = e[0].get<std::string>();
            if (name == "cubicBezier")
                return AnimParam::ease(AnimEasing::cubicBezier(e[1].get<double>(), e[2].get<double>(), e[3].get<double>(), e[4].get<double>()));
            if (name == "steps")
                return AnimParam::ease(AnimEasing::steps(e[1].get<double>(), e.size() > 2 && e[2].get<bool>()));
            std::vector<AnimLinearPoint> points;
            for (size_t i = 1; i < e.size(); i++){
                AnimLinearPoint p;
                if (e[i].is_string()){
                    std::string text = e[i].get<std::string>();
                    p.value = AnimMath::parseFloat(text);
                    size_t space = text.find(' ');
                    if (space != std::string::npos){
                        p.hasPercent = true;
                        p.percent = AnimMath::parseFloat(text.substr(space + 1));
                    }
                } else {
                    p.value = e[i].get<double>();
                }
                points.push_back(p);
            }
            return AnimParam::ease(AnimEasing::linear(points));
        }
        if (j.contains("$spring")){
            AnimSpringParams params;
            const json& s = j["$spring"];
            if (s.contains("bounce")) params.bounce = s["bounce"].get<double>();
            if (s.contains("duration")) params.duration = s["duration"].get<double>();
            if (s.contains("mass")) params.mass = s["mass"].get<double>();
            if (s.contains("stiffness")) params.stiffness = s["stiffness"].get<double>();
            if (s.contains("damping")) params.damping = s["damping"].get<double>();
            if (s.contains("velocity")) params.velocity = s["velocity"].get<double>();
            return AnimParam::spring(std::make_shared<AnimSpring>(params));
        }
        if (j.contains("$stagger")){
            const json& s = j["$stagger"];
            return AnimUtils::stagger(fromJson(s[0]), s.size() > 1 ? fromJson(s[1]) : AnimParam::object());
        }
        if (j.contains("$fn")){
            const double k = j["$fn"][1].get<double>();
            return AnimParam::function([k](const AnimFunctionArgs& args){ return AnimParam(args.index * k); });
        }
        if (j.contains("$modifier")){
            const int decimals = j["$modifier"][1].get<int>();
            return AnimParam::ease([decimals](double v){ return AnimMath::round(v, decimals); });
        }
        AnimParam object = AnimParam::object();
        for (auto it = j.begin(); it != j.end(); ++it)
            object.set(it.key(), fromJson(it.value()));
        return object;
    }

    AnimSpringParams springParams(const json& s){
        AnimSpringParams params;
        if (s.contains("bounce")) params.bounce = s["bounce"].get<double>();
        if (s.contains("duration")) params.duration = s["duration"].get<double>();
        if (s.contains("mass")) params.mass = s["mass"].get<double>();
        if (s.contains("stiffness")) params.stiffness = s["stiffness"].get<double>();
        if (s.contains("damping")) params.damping = s["damping"].get<double>();
        if (s.contains("velocity")) params.velocity = s["velocity"].get<double>();
        return params;
    }

    class AnimeReferenceTest : public ::testing::Test {
    protected:
        void SetUp() override{
            ASSERT_FALSE(reference().is_null()) << "anime/reference.json missing: run gen_reference.mjs";
            AnimEngine::instance().reset();
        }
        void TearDown() override{
            AnimEngine::instance().reset();
        }
    };

}

TEST_F(AnimeReferenceTest, Easings){
    const json& easings = reference()["easings"];
    ASSERT_GE(easings.size(), 60u);
    for (auto it = easings.begin(); it != easings.end(); ++it){
        bool ok = false;
        EaseFn ease = AnimEasing::parse(it.key(), &ok);
        ASSERT_TRUE(ok) << it.key();
        ASSERT_EQ(it.value().size(), 101u);
        for (size_t i = 0; i <= 100; i++){
            const double t = (double)i / 100;
            const double expected = number(it.value()[i]);
            ASSERT_TRUE(near(ease(t), expected)) << it.key() << " at " << t << ": " << ease(t) << " != " << expected;
        }
    }
}

TEST_F(AnimeReferenceTest, EaseFunctionsAndUnknownNames){
    // every eases.* name; the parametric ones take parameters
    EXPECT_EQ(AnimEasing::getNames().size(), 2u + 4u * 11u);
    EXPECT_TRUE(AnimEasing::hasParameters("outElastic"));
    EXPECT_TRUE(AnimEasing::hasParameters("in"));
    EXPECT_FALSE(AnimEasing::hasParameters("inQuad"));
    EXPECT_NEAR(AnimEasing::get("outElastic", {1, .6})(0.3), AnimEasing::parse("outElastic(1, .6)")(0.3), 1e-15);

    // unknown: linear with a warning
    bool ok = true;
    EaseFn unknown = AnimEasing::parse("bouncy", &ok);
    EXPECT_FALSE(ok);
    EXPECT_DOUBLE_EQ(unknown(0.25), 0.25);
    EXPECT_FALSE(AnimEasing::isValid("inQuad("));
    EXPECT_TRUE(AnimEasing::isValid("spring(1, 80, 10, 0)"));
}

TEST_F(AnimeReferenceTest, Springs){
    for (const json& s : reference()["springs"]){
        AnimSpring spring(springParams(s["params"]));
        const std::string label = s["params"].dump();
        EXPECT_EQ(spring.getSettlingDuration(), s["settlingDuration"].get<double>()) << label;
        EXPECT_TRUE(near(spring.getSolverDuration(), s["solverDuration"].get<double>())) << label;
        EXPECT_TRUE(near(spring.getBounce(), s["bounce"].get<double>())) << label;
        EXPECT_TRUE(near(spring.getDuration(), s["duration"].get<double>())) << label;
        EXPECT_TRUE(near(spring.getStiffness(), s["stiffness"].get<double>())) << label;
        EXPECT_TRUE(near(spring.getDamping(), s["damping"].get<double>())) << label;
        EXPECT_TRUE(near(spring.getMass(), s["mass"].get<double>())) << label;
        EXPECT_TRUE(near(spring.getVelocity(), s["velocity"].get<double>())) << label;
        for (size_t i = 0; i <= 100; i++){
            const double t = (double)i / 100;
            ASSERT_TRUE(near(spring.ease(t), number(s["samples"][i]))) << label << " at " << t;
        }
    }
}

TEST_F(AnimeReferenceTest, SpringOnCompleteAtPerceivedDuration){
    AnimSpringParams params;
    params.bounce = 0.5;
    params.duration = 400;
    int completions = 0;
    params.onComplete = [&](AnimTimer*){ completions++; };
    AnimSpring spring(params);
    const double perceived = spring.getDuration() / spring.getSettlingDuration();
    spring.ease(perceived * 0.5);
    EXPECT_EQ(completions, 0);
    spring.ease(perceived);
    spring.ease(1);
    EXPECT_EQ(completions, 1);
    // back before the perceived duration, then past it again
    spring.ease(0.1);
    spring.ease(1);
    EXPECT_EQ(completions, 2);
}

TEST_F(AnimeReferenceTest, Staggers){
    for (const json& s : reference()["staggers"]){
        AnimParam fn = AnimUtils::stagger(fromJson(s["value"]), fromJson(s["params"]));
        ASSERT_TRUE(fn.isFunction());
        const int total = s["total"].get<int>();
        AnimTargetList targets;
        for (int i = 0; i < total; i++)
            targets.push_back(std::make_shared<AnimObjectTarget>());
        const std::string label = s["value"].dump() + " " + s["params"].dump();
        for (int i = 0; i < total; i++){
            AnimFunctionArgs args;
            args.target = targets[i];
            args.index = i;
            args.total = total;
            args.targets = &targets;
            AnimParam out = fn.asFunction()(args);
            const json& expected = s["outputs"][i];
            if (expected.is_string()){
                ASSERT_TRUE(out.isString()) << label << " #" << i;
                EXPECT_TRUE(sameText(out.asString(), expected.get<std::string>())) << label << " #" << i << ": " << out.asString();
            } else {
                ASSERT_TRUE(out.isNumber()) << label << " #" << i;
                EXPECT_TRUE(near(out.asNumber(), number(expected))) << label << " #" << i << ": " << out.asNumber() << " != " << number(expected);
            }
        }
    }
}

TEST_F(AnimeReferenceTest, TimelineOffsets){
    for (const json& c : reference()["timelines"]){
        auto tl = Anime::createTimeline(AnimParam::object({{"autoplay", false}, {"defaults", AnimParam::object({{"ease", "linear"}})}}));
        for (const json& op : c["ops"]){
            const std::string kind = op[0].get<std::string>();
            auto arg = [&](size_t i){ return i < op.size() ? fromJson(op[i]) : AnimParam(); };
            if (kind == "add"){
                tl->add(AnimParam::object({{"duration", op[1].get<double>()}}), arg(2));
            } else if (kind == "addDelay"){
                tl->add(AnimParam::object({{"duration", op[1].get<double>()}, {"delay", op[2].get<double>()}}), arg(3));
            } else if (kind == "addStagger"){
                AnimTargetList targets;
                for (int i = 0; i < op[2].get<int>(); i++)
                    targets.push_back(std::make_shared<AnimObjectTarget>(std::initializer_list<std::pair<const std::string, AnimRawValue>>{{"x", AnimRawValue(0.0)}}));
                const json& st = op[3];
                tl->add(targets, AnimParam::object({{"x", 10}, {"duration", op[1].get<double>()}}),
                        AnimUtils::stagger(fromJson(st[0]), st.size() > 1 ? fromJson(st[1]) : AnimParam::object()));
            } else if (kind == "set"){
                auto target = std::make_shared<AnimObjectTarget>(std::initializer_list<std::pair<const std::string, AnimRawValue>>{{"x", AnimRawValue(0.0)}});
                tl->set({target}, AnimParam::object({{"x", 1}}), arg(1));
            } else if (kind == "call"){
                tl->call([](AnimTimer&){}, arg(1));
            } else if (kind == "label"){
                tl->label(op[1].get<std::string>(), arg(2));
            }
        }
        std::vector<AnimTimer*> children;
        for (AnimTimer* child = tl->childHead; child; child = child->next)
            children.push_back(child);
        const std::string name = c["name"].get<std::string>();
        ASSERT_EQ(children.size(), c["offsets"].size()) << name;
        for (size_t i = 0; i < children.size(); i++){
            const json& o = c["offsets"][i];
            EXPECT_TRUE(near(children[i]->offset, o["offset"].get<double>())) << name << " child " << i << ": " << children[i]->offset;
            EXPECT_TRUE(near(children[i]->delay, o["delay"].get<double>())) << name << " child " << i;
            EXPECT_TRUE(near(children[i]->duration, o["duration"].get<double>())) << name << " child " << i;
        }
        EXPECT_TRUE(near(tl->getDuration(), c["duration"].get<double>())) << name << ": " << tl->getDuration();
        EXPECT_TRUE(near(tl->getIterationDuration(), c["iterationDuration"].get<double>())) << name;
        EXPECT_EQ(tl->getLabels().size(), c["labels"].size()) << name;
        for (auto it = c["labels"].begin(); it != c["labels"].end(); ++it)
            EXPECT_TRUE(near(tl->getLabels().at(it.key()), it.value().get<double>())) << name << " label " << it.key();
    }
}

TEST_F(AnimeReferenceTest, TimelinePositions){
    auto tl = Anime::createTimeline(AnimParam::object({{"autoplay", false}}));
    tl->add(AnimParam::object({{"duration", 1000}})).add(AnimParam::object({{"duration", 400}}), 200).label("mid", 500);
    for (const json& p : reference()["positions"]){
        const double value = tl->parsePosition(fromJson(p["position"]));
        EXPECT_TRUE(near(value, number(p["value"]))) << p["position"].dump() << ": " << value;
    }
}

TEST_F(AnimeReferenceTest, ValueDecomposition){
    for (const json& d : reference()["decomposed"]){
        const json& input = d["input"];
        AnimRawValue raw = input.is_number() ? AnimRawValue(input.get<double>()) : AnimRawValue(input.get<std::string>());
        AnimDecomposed out = AnimValue::decompose(raw);
        const std::string label = input.dump();
        EXPECT_EQ((int)out.t, d["t"].get<int>()) << label;
        EXPECT_TRUE(near(out.n, number(d["n"]))) << label << ": " << out.n;
        EXPECT_EQ(out.u, d["u"].is_null() ? std::string() : d["u"].get<std::string>()) << label;
        EXPECT_EQ(out.o ? std::string(1, out.o) : std::string(), d["o"].is_null() ? std::string() : d["o"].get<std::string>()) << label;
        EXPECT_EQ(out.d.has_value(), !d["d"].is_null()) << label;
        if (out.d && !d["d"].is_null()){
            ASSERT_EQ(out.d->size(), d["d"].size()) << label;
            for (size_t i = 0; i < out.d->size(); i++)
                EXPECT_TRUE(near((*out.d)[i], number(d["d"][i]))) << label << " d" << i;
        }
        EXPECT_EQ(out.s.has_value(), !d["s"].is_null()) << label;
        if (out.s && !d["s"].is_null()){
            ASSERT_EQ(out.s->size(), d["s"].size()) << label;
            for (size_t i = 0; i < out.s->size(); i++)
                EXPECT_EQ((*out.s)[i], d["s"][i].get<std::string>()) << label << " s" << i;
        }
    }
}

TEST_F(AnimeReferenceTest, Colors){
    for (const json& c : reference()["colors"]){
        AnimColor color = AnimValue::parseColor(c["input"].get<std::string>());
        for (int i = 0; i < 4; i++)
            EXPECT_TRUE(near(color[i], c["rgba"][i].get<double>())) << c["input"] << " channel " << i;
    }
    EXPECT_EQ(AnimValue::colorToHexString({255, 0, 127.6, 0.5}), "#ff008080");
    EXPECT_EQ(AnimValue::colorToRgbaString({255, 0, 127.6, 0.5}), "rgba(255,0,128,0.5)");
}

TEST_F(AnimeReferenceTest, Utils){
    const json& u = reference()["utils"];
    auto args = [](const json& e){ std::vector<double> a; for (const json& v : e["args"]) a.push_back(v.is_number() ? v.get<double>() : 0); return a; };
    for (const json& e : u["wrap"]){ auto a = args(e); EXPECT_TRUE(near(AnimUtils::wrap(a[0], a[1], a[2]), e["value"].get<double>())) << e.dump(); }
    for (const json& e : u["mapRange"]){ auto a = args(e); EXPECT_TRUE(near(AnimUtils::mapRange(a[0], a[1], a[2], a[3], a[4]), e["value"].get<double>())) << e.dump(); }
    for (const json& e : u["snap"]){
        const json& inc = e["args"][1];
        double v;
        if (inc.is_array()){
            std::vector<double> points;
            for (const json& p : inc) points.push_back(p.get<double>());
            v = AnimUtils::snap(e["args"][0].get<double>(), points);
        } else {
            v = AnimUtils::snap(e["args"][0].get<double>(), inc.get<double>());
        }
        EXPECT_TRUE(near(v, e["value"].get<double>())) << e.dump();
    }
    for (const json& e : u["round"]){ auto a = args(e); EXPECT_TRUE(near(AnimUtils::round(a[0], (int)a[1]), e["value"].get<double>())) << e.dump(); }
    for (const json& e : u["clamp"]){ auto a = args(e); EXPECT_TRUE(near(AnimUtils::clamp(a[0], a[1], a[2]), e["value"].get<double>())) << e.dump(); }
    for (const json& e : u["lerp"]){ auto a = args(e); EXPECT_TRUE(near(AnimUtils::lerp(a[0], a[1], a[2]), e["value"].get<double>())) << e.dump(); }
    for (const json& e : u["damp"]){ auto a = args(e); EXPECT_TRUE(near(AnimUtils::damp(a[0], a[1], a[2], a[3]), e["value"].get<double>())) << e.dump(); }
    for (const json& e : u["degToRad"]){ auto a = args(e); EXPECT_TRUE(near(AnimUtils::degToRad(a[0]), e["value"].get<double>())) << e.dump(); }
    for (const json& e : u["radToDeg"]){ auto a = args(e); EXPECT_TRUE(near(AnimUtils::radToDeg(a[0]), e["value"].get<double>())) << e.dump(); }
    for (const json& e : u["roundPad"]){ auto a = args(e); EXPECT_EQ(AnimUtils::roundPad(a[0], (int)a[1]), e["value"].get<std::string>()) << e.dump(); }
    auto text = [](const json& v){ return v.is_string() ? v.get<std::string>() : AnimMath::toString(v.get<double>()); };
    for (const json& e : u["padStart"]) EXPECT_EQ(AnimUtils::padStart(text(e["args"][0]), e["args"][1].get<int>(), e["args"][2].get<std::string>()), e["value"].get<std::string>()) << e.dump();
    for (const json& e : u["padEnd"]) EXPECT_EQ(AnimUtils::padEnd(text(e["args"][0]), e["args"][1].get<int>(), e["args"][2].get<std::string>()), e["value"].get<std::string>()) << e.dump();
    AnimRandomFn seeded = AnimUtils::createSeededRandom(1234);
    for (size_t i = 0; i < u["seededRandom"].size(); i++)
        EXPECT_TRUE(near(seeded(0, 100, (int)(i % 3)), u["seededRandom"][i].get<double>())) << "seeded #" << i;

    for (int i = 0; i < 200; i++){
        const double r = AnimUtils::random(-2, 3, 1);
        EXPECT_GE(r, -2);
        EXPECT_LE(r, 3);
        EXPECT_NEAR(r * 10, std::round(r * 10), 1e-9);
    }
}

TEST_F(AnimeReferenceTest, NumberToString){
    EXPECT_EQ(AnimMath::toString(0.1 + 0.2), "0.30000000000000004");
    EXPECT_EQ(AnimMath::toString(100), "100");
    EXPECT_EQ(AnimMath::toString(-12.5), "-12.5");
    EXPECT_EQ(AnimMath::toString(1e-7), "1e-7");
    EXPECT_EQ(AnimMath::toString(1e21), "1e+21");
    EXPECT_EQ(AnimMath::toString(123456789012), "123456789012");
    EXPECT_EQ(AnimMath::toString(0.000001), "0.000001");
    EXPECT_TRUE(std::isnan(AnimMath::toNumber("12px")));
    EXPECT_EQ(AnimMath::toNumber("  12.5 "), 12.5);
    EXPECT_EQ(AnimMath::toNumber(""), 0);
    EXPECT_EQ(AnimMath::parseFloat("12.5px"), 12.5);
    EXPECT_EQ(AnimMath::jsRound(-1.5), -1);
    EXPECT_EQ(AnimMath::jsRound(2.5), 3);
    EXPECT_EQ(AnimMath::jsRound(0.49999999999999994), 0);
}

// ---------------------------------------------------------------------------------------------
// recordings

namespace {

    const char* callbackNames[] = {"onBegin", "onBeforeUpdate", "onUpdate", "onRender", "onLoop", "onPause", "onComplete"};

    struct Recorder {
        std::vector<std::string> log;

        AnimParam withCallbacks(const std::string& id, const json& params){
            AnimParam p = fromJson(params);
            if (!p.isObject())
                p = AnimParam::object();
            for (const char* cb : callbackNames){
                std::string entry = id + "." + cb;
                p.set(cb, AnimParam::callback([this, entry](AnimTimer&){ log.push_back(entry); }));
            }
            return p;
        }
    };

}

TEST_F(AnimeReferenceTest, Recordings){
    // gen_reference.mjs's clock: 1000 at Anime's import (its engine's start), +10000 before
    // each recording, +dt per frame; the engine's clock starts at 0
    double now = 0;
    size_t compared = 0, callbacksCompared = 0;
    AnimEngine& engine = AnimEngine::instance();
    for (const json& c : reference()["recordings"]){
        const std::string name = c["name"].get<std::string>();
        SCOPED_TRACE(name);
        std::map<std::string, std::shared_ptr<AnimObjectTarget>> targets;
        for (auto it = c["targets"].begin(); it != c["targets"].end(); ++it){
            auto target = std::make_shared<AnimObjectTarget>();
            for (auto v = it.value().begin(); v != it.value().end(); ++v)
                target->set(v.key(), v.value().is_number() ? AnimRawValue(v.value().get<double>()) : AnimRawValue(v.value().get<std::string>()));
            targets[it.key()] = target;
        }
        auto targetList = [&](const json& names){
            AnimTargetList list;
            for (const json& n : names) list.push_back(targets[n.get<std::string>()]);
            return list;
        };
        Recorder recorder;
        std::map<std::string, std::shared_ptr<AnimTimer>> objects;
        std::vector<std::string> objectOrder;
        now += 10000;
        engine.tickMilliseconds(now, 0);
        const double dt = c["dt"].get<double>();
        std::ostringstream mismatches;
        int mismatchCount = 0;
        const int frames = c["frames"].get<int>();
        for (int frame = 0; frame < frames; frame++){
            for (const json& step : c["steps"]){
                if (step["frame"].get<int>() != frame)
                    continue;
                const std::string op = step["op"].get<std::string>();
                const std::string id = step.value("id", "");
                if (op == "animate"){
                    auto animation = Anime::animate(targetList(step["targets"]), recorder.withCallbacks(id, step["params"]));
                    std::string entry = id + ".then";
                    animation->then([&recorder, entry](AnimTimer&){ recorder.log.push_back(entry); });
                    objects[id] = animation;
                } else if (op == "timer"){
                    objects[id] = Anime::createTimer(recorder.withCallbacks(id, step["params"]));
                } else if (op == "timeline"){
                    auto tl = Anime::createTimeline(recorder.withCallbacks(id, step["params"]));
                    for (const json& child : step["children"]){
                        const std::string kind = child[0].get<std::string>();
                        const std::string childId = child[1].get<std::string>();
                        auto arg = [&](size_t i){ return i < child.size() ? fromJson(child[i]) : AnimParam(); };
                        if (kind == "add") tl->add(targetList(child[2]), recorder.withCallbacks(childId, child[3]), arg(4));
                        else if (kind == "set") tl->set(targetList(child[2]), fromJson(child[3]), arg(4));
                        else if (kind == "label") tl->label(childId, arg(2));
                        else if (kind == "call"){
                            std::string entry = childId + ".call";
                            tl->call([&recorder, entry](AnimTimer&){ recorder.log.push_back(entry); }, arg(2));
                        }
                        else if (kind == "timer") tl->add(recorder.withCallbacks(childId, child[2]), arg(3));
                        else if (kind == "sync") tl->sync(objects[childId], arg(2));
                    }
                    objects[id] = tl;
                } else if (op == "call"){
                    AnimTimer& o = *objects[id];
                    const std::string method = step["method"].get<std::string>();
                    const json args = step.value("args", json::array());
                    if (method == "pause") o.pause();
                    else if (method == "resume") o.resume();
                    else if (method == "play") o.play();
                    else if (method == "reverse") o.reverse();
                    else if (method == "restart") o.restart();
                    else if (method == "alternate") o.alternate();
                    else if (method == "seek") o.seek(args[0].get<double>());
                    else if (method == "stretch") o.stretch(args[0].get<double>());
                    else if (method == "complete") o.complete();
                    else if (method == "revert") o.revert();
                    else if (method == "cancel") o.cancel();
                    else FAIL() << "unknown method " << method;
                } else if (op == "setProp"){
                    AnimTimer& o = *objects[id];
                    const std::string prop = step["prop"].get<std::string>();
                    const double value = step["value"].get<double>();
                    if (prop == "progress") o.setProgress(value);
                    else if (prop == "currentTime") o.setCurrentTime(value);
                    else if (prop == "speed") o.setSpeed(value);
                    else FAIL() << "unknown property " << prop;
                }
                if (objects.count(id) && std::find(objectOrder.begin(), objectOrder.end(), id) == objectOrder.end())
                    objectOrder.push_back(id);
            }
            now += dt;
            engine.tickMilliseconds(now, 0);

            const json& expected = c["recorded"][frame];
            for (auto t = expected["values"].begin(); t != expected["values"].end(); ++t){
                for (auto v = t.value().begin(); v != t.value().end(); ++v){
                    AnimRawValue actual = targets[t.key()]->get(v.key());
                    compared++;
                    bool same;
                    if (v.value().is_string()){
                        same = actual.isString() && sameText(actual.text, v.value().get<std::string>());
                    } else {
                        same = actual.isNumber() && near(actual.number, number(v.value()));
                    }
                    if (!same && mismatchCount++ < 12)
                        mismatches << "frame " << frame << " " << t.key() << "." << v.key() << ": " << describe(actual) << " != " << v.value().dump() << "\n";
                }
            }
            std::vector<std::string> expectedLog;
            for (const json& entry : expected["callbacks"]) expectedLog.push_back(entry.get<std::string>());
            callbacksCompared += expectedLog.size();
            if (recorder.log != expectedLog && mismatchCount++ < 12){
                mismatches << "frame " << frame << " callbacks:\n  got      ";
                for (const std::string& s : recorder.log) mismatches << s << " ";
                mismatches << "\n  expected ";
                for (const std::string& s : expectedLog) mismatches << s << " ";
                mismatches << "\n";
            }
            recorder.log.clear();
            for (auto s = expected["state"].begin(); s != expected["state"].end(); ++s){
                AnimTimer& o = *objects[s.key()];
                const json& st = s.value();
                std::ostringstream diff;
                if (!near(o.getCurrentTime(), number(st["currentTime"]), 1e-7)) diff << " currentTime " << o.getCurrentTime() << " != " << st["currentTime"];
                if (!near(o.getProgress(), number(st["progress"]), 1e-7)) diff << " progress " << o.getProgress() << " != " << st["progress"];
                if (o.getCurrentIteration() != st["iteration"].get<int>()) diff << " iteration " << o.getCurrentIteration() << " != " << st["iteration"];
                if (o.isPaused() != st["paused"].get<bool>()) diff << " paused " << o.isPaused();
                if (o.hasBegun() != st["began"].get<bool>()) diff << " began " << o.hasBegun();
                if (o.isCompleted() != st["completed"].get<bool>()) diff << " completed " << o.isCompleted();
                if (o.isReversed() != st["reversed"].get<bool>()) diff << " reversed " << o.isReversed();
                if (!diff.str().empty() && mismatchCount++ < 12)
                    mismatches << "frame " << frame << " " << s.key() << ":" << diff.str() << "\n";
            }
        }
        for (const std::string& id : objectOrder)
            objects[id]->cancel();
        EXPECT_EQ(mismatchCount, 0) << mismatches.str();
    }
    // the recordings really compared something
    EXPECT_GT(compared, 2000u);
    EXPECT_GT(callbacksCompared, 3000u);
}
