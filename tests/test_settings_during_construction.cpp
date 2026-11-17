#include "SettingsDuringConstruction.h"

#include <catch2/catch_test_macros.hpp>

using dispatcharr::ConstructionDeliveryTracker;

TEST_CASE("ConstructionDeliveryTracker: a delivery from another thread mid-construction is reported once",
          "[settings_during_construction]")
{
  ConstructionDeliveryTracker<int> tracker;
  tracker.BeginConstruction(1);
  CHECK(tracker.Constructing());
  tracker.OnDelivery(2);
  CHECK(tracker.EndConstruction(1));
  CHECK_FALSE(tracker.Constructing());
  // Ended: a second end, or a later delivery, reports nothing.
  CHECK_FALSE(tracker.EndConstruction(1));
  tracker.OnDelivery(2);
  CHECK_FALSE(tracker.Constructing());
}

TEST_CASE("ConstructionDeliveryTracker: the constructor's own writes are not a missed delivery",
          "[settings_during_construction]")
{
  ConstructionDeliveryTracker<int> tracker;
  tracker.BeginConstruction(1);
  tracker.OnDelivery(1);
  tracker.OnDelivery(1);
  CHECK_FALSE(tracker.EndConstruction(1));
}

TEST_CASE("ConstructionDeliveryTracker: deliveries before or after a construction are not counted",
          "[settings_during_construction]")
{
  ConstructionDeliveryTracker<int> tracker;
  tracker.OnDelivery(2);
  tracker.BeginConstruction(1);
  CHECK_FALSE(tracker.EndConstruction(1));
  tracker.OnDelivery(2);
  tracker.BeginConstruction(1);
  CHECK_FALSE(tracker.EndConstruction(1));
}

TEST_CASE("ConstructionDeliveryTracker: concurrent constructions are tracked separately",
          "[settings_during_construction]")
{
  ConstructionDeliveryTracker<int> tracker;
  tracker.BeginConstruction(1);
  tracker.BeginConstruction(2);
  // Thread 2's own write counts against construction 1 only.
  tracker.OnDelivery(2);
  CHECK_FALSE(tracker.EndConstruction(2));
  CHECK(tracker.Constructing());
  CHECK(tracker.EndConstruction(1));
  CHECK_FALSE(tracker.Constructing());
}

TEST_CASE("ConstructionDeliveryTracker: ending a construction never begun changes nothing",
          "[settings_during_construction]")
{
  ConstructionDeliveryTracker<int> tracker;
  tracker.BeginConstruction(1);
  tracker.OnDelivery(2);
  CHECK_FALSE(tracker.EndConstruction(3));
  CHECK(tracker.EndConstruction(1));
}
