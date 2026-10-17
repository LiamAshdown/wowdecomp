-- One-time setup after the first `docker compose up` (once ac-db-import has finished):
--   docker compose exec -T ac-database sh -c 'mysql -uroot -p"$MYSQL_ROOT_PASSWORD"' \
--       < <(sed "s/@BRIDGE_DB_PASSWORD@/$BRIDGE_DB_PASSWORD/" setup.sql)
-- The realm address stays 127.0.0.1: the bridge maps it to the worldserver container.
CREATE USER IF NOT EXISTS 'wowbridge'@'%' IDENTIFIED BY '@BRIDGE_DB_PASSWORD@';
GRANT SELECT, INSERT, UPDATE ON acore_auth.account TO 'wowbridge'@'%';
GRANT SELECT, INSERT ON acore_auth.realmcharacters TO 'wowbridge'@'%';
GRANT SELECT ON acore_auth.realmlist TO 'wowbridge'@'%';
GRANT SELECT ON acore_auth.account_banned TO 'wowbridge'@'%';
-- The admin page (Tools/Bridge/admin.mjs): characters to read, and the anonymous per-day usage counts.
GRANT SELECT ON acore_characters.characters TO 'wowbridge'@'%';
CREATE DATABASE IF NOT EXISTS lasthavens;
CREATE TABLE IF NOT EXISTS lasthavens.web_events (
    day DATE NOT NULL,
    event VARCHAR(32) NOT NULL,
    detail VARCHAR(32) NOT NULL DEFAULT '',
    count INT UNSIGNED NOT NULL DEFAULT 0,
    total_ms BIGINT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (day, event, detail)
);
GRANT SELECT, INSERT, UPDATE ON lasthavens.web_events TO 'wowbridge'@'%';
-- icon 1 = PvP, matching AC_GAME_TYPE in docker-compose.yml.
UPDATE acore_auth.realmlist SET name = 'Lasthavens', address = '127.0.0.1', port = 8085, icon = 1 WHERE id = 1;
-- Read when the worldserver starts.
UPDATE acore_auth.motd SET text = 'Welcome to Lasthavens! Join the community on Discord: https://discord.gg/2EZTV34rfN' WHERE realmid = -1;
