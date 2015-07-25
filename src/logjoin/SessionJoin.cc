/**
 * Copyright (c) 2015 - The CM Authors <legal@clickmatcher.com>
 *   All Rights Reserved.
 *
 * This file is CONFIDENTIAL -- Distribution or duplication of this material or
 * the information contained herein is strictly forbidden unless prior written
 * permission is obtained.
 */
#include "logjoin/SessionJoin.h"
#include "logjoin/common.h"

using namespace stx;

namespace cm {

void SessionJoin::process(RefPtr<TrackedSessionContext> ctx) {

  /* load builtin events into structured format */
  std::vector<TrackedQuery> queries;
  std::vector<TrackedItemVisit> page_views;
  std::vector<TrackedCartItem> cart_items;

  for (const auto& ev : ctx->tracked_session.events) {
    if (ev.evtype == "_search_query") {
      processSearchQueryEvent(ev, &queries);
      continue;
    }

    if (ev.evtype == "_pageview") {
      processPageViewEvent(ev, &page_views);
      continue;
    }

    if (ev.evtype == "_cart_items") {
      processCartItemsEvent(ev, &cart_items);
      continue;
    }
  }

  /* update queries (mark items as clicked) */
  for (auto& cur_query : queries) {

    /* search for matching item visits */
    for (auto& cur_visit : page_views) {
      auto cutoff = cur_query.time.unixMicros() +
          kMaxQueryClickDelaySeconds * kMicrosPerSecond;

      if (cur_visit.time < cur_query.time ||
          cur_visit.time.unixMicros() > cutoff) {
        continue;
      }

      for (auto& qitem : cur_query.items) {
        if (cur_visit.item == qitem.item) {
          qitem.clicked = true;
          qitem.seen = true;
          break;
        }
      }
    }
  }

  /* calculate global gmv */
  uint32_t num_cart_items = 0;
  uint32_t num_order_items = 0;
  uint32_t gmv_eurcents = 0;
  uint32_t cart_value_eurcents = 0;
  HashMap<String, uint64_t> cart_eurcents_per_item;
  HashMap<String, uint64_t> gmv_eurcents_per_item;
  for (const auto& ci : cart_items) {
    auto currency = currencyFromString(ci.currency);
    auto eur = cconv()->convert(Money(ci.price_cents, currency), CURRENCY_EUR);
    auto eurcents = eur.cents;
    eurcents *= ci.quantity;
    cart_eurcents_per_item.emplace(ci.item.docID().docid, eurcents);

    ++num_cart_items;
    cart_value_eurcents += eurcents;
    if (ci.checkout_step == 1) {
      gmv_eurcents_per_item.emplace(ci.item.docID().docid, eurcents);
      ++num_order_items;
      gmv_eurcents += eurcents;
    }
  }

  /* calculate gmv and ctrs per query */
  for (auto& q : queries) {
    auto slrid = extractAttr(q.attrs, "slrid");

    for (auto& i : q.items) {
      // DAWANDA HACK
      if (i.position >= 1 && i.position <= 4 && slrid.isEmpty()) {
        ++q.nads;
        q.nadclicks += i.clicked;
      }
      // EOF DAWANDA HACK

      ++q.nitems;

      if (i.clicked) {
        ++q.nclicks;

        {
          auto ci = cart_eurcents_per_item.find(i.item.docID().docid);
          if (ci != cart_eurcents_per_item.end()) {
            ++q.num_cart_items;
            q.cart_value_eurcents += ci->second;
          }
        }

        {
          auto ci = gmv_eurcents_per_item.find(i.item.docID().docid);
          if (ci != gmv_eurcents_per_item.end()) {
            ++q.num_order_items;
            q.gmv_eurcents += ci->second;
          }
        }
      }
    }
  }

  for (const auto& q : queries) {
    auto qobj = ctx->joined_session.add_search_queries();

    qobj->set_time(q.time.unixMicros() / kMicrosPerSecond);
    qobj->set_language((ProtoLanguage) cm::extractLanguage(q.attrs));

    auto qstr = cm::extractQueryString(q.attrs);
    if (!qstr.isEmpty()) {
      qobj->set_query_string(qstr.get());
    }

    auto slrid = cm::extractAttr(q.attrs, "slrid");
    if (!slrid.isEmpty()) {
      try {
        uint32_t sid = std::stoul(slrid.get());
        qobj->set_shop_id(sid);
      } catch (...) {}
    }

    qobj->set_num_result_items(q.nitems);
    qobj->set_num_result_items_clicked(q.nclicks);
    qobj->set_num_ad_impressions(q.nads);
    qobj->set_num_ad_clicks(q.nadclicks);
    qobj->set_num_cart_items(q.num_cart_items);
    qobj->set_cart_value_eurcents(q.cart_value_eurcents);
    qobj->set_num_order_items(q.num_order_items);
    qobj->set_gmv_eurcents(q.gmv_eurcents);

    auto pg_str = cm::extractAttr(q.attrs, "pg");
    if (!pg_str.isEmpty()) {
      try {
        auto val = std::stoull(pg_str.get());
        qobj->set_page(val);
      } catch (...) {}
    }

    auto abgrp = cm::extractABTestGroup(q.attrs);
    if (!abgrp.isEmpty()) {
      qobj->set_ab_test_group(abgrp.get());
    }

    auto qcat1 = cm::extractAttr(q.attrs, "q_cat1");
    if (!qcat1.isEmpty()) {
      try {
        auto val = std::stoull(qcat1.get());
        qobj->set_category1(val);
      } catch (...) {}
    }

    auto qcat2 = cm::extractAttr(q.attrs, "q_cat2");
    if (!qcat2.isEmpty()) {
      try {
        auto val = std::stoull(qcat2.get());
        qobj->set_category2(val);
      } catch (...) {}
    }

    auto qcat3 = cm::extractAttr(q.attrs, "q_cat3");
    if (!qcat3.isEmpty()) {
      try {
        auto val = std::stoull(qcat3.get());
        qobj->set_category3(val);
      } catch (...) {}
    }

    qobj->set_device_type((ProtoDeviceType) extractDeviceType(q.attrs));
    qobj->set_page_type((ProtoPageType) extractPageType(q.attrs));

    String query_type = pageTypeToString((PageType) qobj->page_type());
    auto qtype_attr = cm::extractAttr(q.attrs, "qt");
    if (!qtype_attr.isEmpty()) {
      query_type = qtype_attr.get();
    }

    qobj->set_query_type(query_type);

    for (const auto& item : q.items) {
      auto item_obj = qobj->add_result_items();

      item_obj->set_position(item.position);
      item_obj->set_item_id(item.item.docID().docid);
      item_obj->set_clicked(item.clicked);
      item_obj->set_seen(item.seen);
    }
  }

  for (const auto& iv : page_views) {
    auto ivobj = ctx->joined_session.add_item_visits();

    ivobj->set_time(iv.time.unixMicros() / kMicrosPerSecond);
    ivobj->set_item_id(iv.item.docID().docid);
  }

  for (const auto& ev : ctx->tracked_session.events) {
    if (ev.evtype != "__sattr") {
      continue;
    }

    URI::ParamList logline;
    URI::parseQueryString(ev.data, &logline);

    //for (const auto& p : logline) {
    //  if (p.first == "x") {
    //    experiments.emplace(p.second);
    //    continue;
    //  }
    //}

    std::string r_url;
    if (stx::URI::getParam(logline, "r_url", &r_url)) {
      ctx->joined_session.set_referrer_url(r_url);
    }

    std::string r_cpn;
    if (stx::URI::getParam(logline, "r_cpn", &r_cpn)) {
      ctx->joined_session.set_referrer_campaign(r_cpn);
    }

    std::string r_nm;
    if (stx::URI::getParam(logline, "r_nm", &r_nm)) {
      ctx->joined_session.set_referrer_name(r_nm);
    }

    std::string cs;
    if (stx::URI::getParam(logline, "cs", &cs)) {
      ctx->joined_session.set_customer_session_id(cs);
    }
  }

  ctx->joined_session.set_num_cart_items(num_cart_items);
  ctx->joined_session.set_cart_value_eurcents(cart_value_eurcents);
  ctx->joined_session.set_num_order_items(num_order_items);
  ctx->joined_session.set_gmv_eurcents(gmv_eurcents);

  auto first_seen = firstSeenTime(ctx->joined_session);
  auto last_seen = lastSeenTime(ctx->joined_session);
  if (first_seen.isEmpty() || last_seen.isEmpty()) {
    RAISE(kRuntimeError, "session: time isn't set");
  }

  ctx->joined_session.set_first_seen_time(
      first_seen.get().unixMicros() / kMicrosPerSecond);

  ctx->joined_session.set_last_seen_time(
      last_seen.get().unixMicros() / kMicrosPerSecond);

  ctx->joined_session.set_customer(ctx->tracked_session.customer_key);
}

void SessionJoin::processSearchQueryEvent(
    const TrackedEvent& event,
    Vector<TrackedQuery>* queries) {
  TrackedQuery query;
  query.time = event.time;
  query.eid = event.evid;

  URI::ParamList logline;
  URI::parseQueryString(event.data, &logline);
  query.fromParams(logline);

  for (auto& q : *queries) {
    if (q.eid == query.eid) {
      q.merge(query);
      return;
    }
  }

  queries->emplace_back(query);
}

void SessionJoin::processPageViewEvent(
    const TrackedEvent& event,
    Vector<TrackedItemVisit>* page_views) {
  TrackedItemVisit visit;
  visit.time = event.time;
  visit.eid = event.evid;

  URI::ParamList logline;
  URI::parseQueryString(event.data, &logline);
  visit.fromParams(logline);

  for (auto& v : *page_views) {
    if (v.eid == visit.eid) {
      v.merge(visit);
      return;
    }
  }

  page_views->emplace_back(visit);
}

void SessionJoin::processCartItemsEvent(
    const TrackedEvent& event,
    Vector<TrackedCartItem>* cart_items) {
  URI::ParamList logline;
  URI::parseQueryString(event.data, &logline);

  auto new_cart_items = TrackedCartItem::fromParams(logline);
  for (auto& ci : new_cart_items) {
    ci.time = event.time;
  }

  for (const auto& cart_item : new_cart_items) {
    bool merged = false;

    for (auto& c : *cart_items) {
      if (c.item == cart_item.item) {
        c.merge(cart_item);
        merged = true;
        break;
      }
    }

    if (!merged) {
      cart_items->emplace_back(cart_item);
    }
  }
}

Option<UnixTime> SessionJoin::firstSeenTime(const JoinedSession& sess) {
  uint64_t t = std::numeric_limits<uint64_t>::max();

  for (const auto& e : sess.search_queries()) {
    if (e.time() * kMicrosPerSecond < t) {
      t = e.time() * kMicrosPerSecond;
    }
  }

  for (const auto& e : sess.item_visits()) {
    if (e.time() * kMicrosPerSecond < t) {
      t = e.time() * kMicrosPerSecond;
    }
  }

  for (const auto& e : sess.cart_items()) {
    if (e.time() * kMicrosPerSecond < t) {
      t = e.time() * kMicrosPerSecond;
    }
  }

  if (t == std::numeric_limits<uint64_t>::max()) {
    return None<UnixTime>();
  } else {
    return Some(UnixTime(t));
  }
}

Option<UnixTime> SessionJoin::lastSeenTime(const JoinedSession& sess) {
  uint64_t t = std::numeric_limits<uint64_t>::min();

  for (const auto& e : sess.search_queries()) {
    if (e.time() * kMicrosPerSecond > t) {
      t = e.time() * kMicrosPerSecond;
    }
  }

  for (const auto& e : sess.item_visits()) {
    if (e.time() * kMicrosPerSecond > t) {
      t = e.time() * kMicrosPerSecond;
    }
  }

  for (const auto& e : sess.cart_items()) {
    if (e.time() * kMicrosPerSecond > t) {
      t = e.time() * kMicrosPerSecond;
    }
  }

  if (t == std::numeric_limits<uint64_t>::min()) {
    return None<UnixTime>();
  } else {
    return Some(UnixTime(t));
  }
}

} // namespace cm

