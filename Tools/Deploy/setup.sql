-- One-time setup after the first `docker compose up` (once ac-db-import has finished):
--   docker compose exec -T ac-database sh -c 'mysql -uroot -p"$MYSQL_ROOT_PASSWORD"' \
--       < <(sed "s/@BRIDGE_DB_PASSWORD@/$BRIDGE_DB_PASSWORD/" setup.sql)
-- The realm address stays 127.0.0.1: the bridge maps it to the worldserver container.
CREATE USER IF NOT EXISTS 'wowbridge'@'%' IDENTIFIED BY '@BRIDGE_DB_PASSWORD@';
GRANT SELECT, INSERT, UPDATE ON acore_auth.account TO 'wowbridge'@'%';
GRANT SELECT, INSERT ON acore_auth.realmcharacters TO 'wowbridge'@'%';
GRANT SELECT ON acore_auth.realmlist TO 'wowbridge'@'%';
GRANT SELECT ON acore_auth.account_banned TO 'wowbridge'@'%';
UPDATE acore_auth.realmlist SET name = 'Lasthavens', address = '127.0.0.1', port = 8085 WHERE id = 1;
