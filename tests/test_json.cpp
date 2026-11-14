#include "test.h"

#include "core/hlx.h"
#include "core/json.h"

TEST("json: an .hlx-shaped document") {
    const char* text = R"({
 "data" : {
  "device" : 2162692,
  "meta" : { "name" : "Café \"x\"" },
  "tone" : {
   "dsp0" : { "block0" : { "@enabled" : false, "@position" : 3, "Mix" : 0.5, "BBD Size" : 2 } },
   "list" : [ 1, -2.5e1, true, null, [] ]
  }
 },
 "schema" : "L6Preset"
}
)";
    std::string error;
    const auto v = hx::json::parse(text, &error);
    if (!v) std::printf("  %s\n", error.c_str());
    REQUIRE(v.has_value());
    const hx::json::Value* data = v->get("data");
    REQUIRE(data != nullptr);
    CHECK_EQ(data->get("device")->asInt(), std::int64_t(2162692));
    CHECK(data->get("device")->integer);
    CHECK_EQ(data->get("meta")->get("name")->asString(), std::string("Caf\xc3\xa9 \"x\""));
    const hx::json::Value* block = data->get("tone")->get("dsp0")->get("block0");
    REQUIRE(block != nullptr);
    CHECK(!block->get("@enabled")->asBool(true));
    CHECK_EQ(block->get("@position")->asInt(), std::int64_t(3));
    CHECK_EQ(block->get("Mix")->asNumber(), 0.5);
    CHECK(!block->get("Mix")->integer);
    CHECK(block->get("BBD Size") != nullptr);
    CHECK_EQ(block->keys[2], std::string("Mix"));  // file order kept
    const hx::json::Value* list = data->get("tone")->get("list");
    REQUIRE(list != nullptr && list->isArray());
    REQUIRE(list->items.size() == 5);
    CHECK_EQ(list->items[1].asNumber(), -25.0);
    CHECK(list->items[2].asBool());
    CHECK(list->items[3].type == hx::json::Type::Null);
    CHECK(list->items[4].isArray() && list->items[4].items.empty());
    CHECK(v->get("missing") == nullptr);
}

TEST("json: bad input is refused with a reason") {
    std::string error;
    CHECK(!hx::json::parse("{\"a\": }", &error).has_value());
    CHECK(!error.empty());
    CHECK(!hx::json::parse("[1, 2", nullptr).has_value());
    CHECK(!hx::json::parse("\"open", nullptr).has_value());
    CHECK(!hx::json::parse("{} x", nullptr).has_value());
    CHECK(hx::json::parse("  \xef\xbb\xbf", nullptr) == std::nullopt);  // a BOM alone is no value
    CHECK(hx::json::parse("\xef\xbb\xbf{}", nullptr).has_value());
}

TEST("json: write reads back the same") {
    using hx::json::Value;
    Value o = Value::object();
    o.set("name", Value::string("Café \"x\"\n"));
    o.set("int", Value::integerNumber(-3));
    o.set("real", Value::real(1.0));
    o.set("float", Value::real(double(0.1f)));
    o.set("on", Value::boolean(true));
    Value& a = o.set("list", Value::array());
    a.push(Value::integerNumber(1));
    a.push(Value::object());
    o.set("int", Value::integerNumber(7));  // replaces, keeps the place
    const std::string text = hx::json::write(o);
    // Reals keep a fraction, floats come out short.
    CHECK(text.find("\"real\": 1.0") != std::string::npos);
    CHECK(text.find("\"float\": 0.1\n") != std::string::npos || text.find("\"float\": 0.1,") != std::string::npos);
    const auto back = hx::json::parse(text);
    REQUIRE(back.has_value());
    REQUIRE(back->keys.size() == 6);
    CHECK_EQ(back->keys[1], std::string("int"));
    CHECK_EQ(back->get("int")->asInt(), std::int64_t(7));
    CHECK_EQ(back->get("name")->asString(), std::string("Café \"x\"\n"));
    CHECK(!back->get("real")->integer);
    CHECK(back->get("on")->asBool());
    CHECK_EQ(back->get("list")->items.size(), std::size_t(2));
}

TEST("hlx: topology names") {
    CHECK_EQ(std::string(hx::hlxTopology(0)), std::string("A"));
    CHECK_EQ(std::string(hx::hlxTopology(1)), std::string("SABJ"));
    CHECK_EQ(std::string(hx::hlxTopology(2)), std::string("SAB"));
}
