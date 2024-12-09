-- SELECT
--     repo,
--     year,
--     cnt
-- FROM
-- (
--     SELECT
--         row_number() OVER (PARTITION BY year ORDER BY cnt DESC) AS r,
--         repo,
--         year,
--         cnt
--     FROM
--     (
--         SELECT
--         lower(repo_name) AS repo,
--         year(created_at) AS year,
--         count() AS cnt
--         FROM github_events
--         WHERE (event_type = 'WatchEvent') AND (year(created_at) >= 2015)
--         GROUP BY
--             repo,
--             year
--     ) t
-- ) t2
-- WHERE r <= 10
-- ORDER BY
--     year ASC,
--     cnt DESC
