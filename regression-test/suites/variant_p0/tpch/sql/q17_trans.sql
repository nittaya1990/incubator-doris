-- ERROR: correlationFilter can't be null in correlatedToJoin
-- SELECT SUM(CAST(L.var["L_EXTENDEDPRICE"] AS DOUBLE)) / 7.0 AS AVG_YEARLY
-- FROM
--   lineitem L,
--   part P
-- WHERE
--   CAST(P.var["P_PARTKEY"] AS INT) = CAST(L.var["L_PARTKEY"] AS INT)
--   AND CAST(P.var["P_BRAND"] AS TEXT) = 'BRAND#23'
--   AND CAST(P.var["P_CONTAINER"] AS TEXT) = 'MED BOX'
--   AND CAST(L.var["L_QUANTITY"] AS DOUBLE) < (
--     SELECT 0.2 * AVG(CAST(LL.var["L_QUANTITY"] AS DOUBLE))
--     FROM
--       lineitem LL
--     WHERE
--       CAST(LL.var["L_PARTKEY"] AS INT) = CAST(P.var["P_PARTKEY"] AS INT)
--   )