@echo off
SET android64=build-android64\iandyhd3.persistent_pickup_ids.geode
SET win=build\iandyhd3.persistent_pickup_ids.geode

call geode package merge %android64% %win%