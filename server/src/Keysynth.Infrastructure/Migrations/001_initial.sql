-- keysynth schema v1 (docs/specs/2026-10-12-cloud-api.md §5.2). schema_migrations is created by MigrationRunner.

CREATE TABLE users (
  id              BIGINT       NOT NULL AUTO_INCREMENT PRIMARY KEY,
  telegram_id     BIGINT       NOT NULL,
  auth_user_id    VARCHAR(64)  NOT NULL,
  name            VARCHAR(255) NOT NULL DEFAULT '',
  created_at      DATETIME(3)  NOT NULL,
  last_app_ok_at  DATETIME(3)  NOT NULL,
  UNIQUE KEY ux_users_telegram (telegram_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE devices (
  id            BIGINT       NOT NULL AUTO_INCREMENT PRIMARY KEY,
  user_id       BIGINT       NOT NULL,
  device_id     CHAR(36)     NOT NULL,
  name          VARCHAR(100) NOT NULL,
  token_hash    BINARY(32)   NOT NULL,
  created_at    DATETIME(3)  NOT NULL,
  expires_at    DATETIME(3)  NOT NULL,
  last_seen_at  DATETIME(3)  NULL,
  UNIQUE KEY ux_devices_user_device (user_id, device_id),
  UNIQUE KEY ux_devices_token (token_hash),
  CONSTRAINT fk_devices_user FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE presets (
  user_id     BIGINT        NOT NULL,
  slug        VARCHAR(64)   CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  revision    INT UNSIGNED  NOT NULL,
  updated_at  DATETIME(3)   NOT NULL,
  deleted     TINYINT(1)    NOT NULL DEFAULT 0,
  patch       MEDIUMTEXT    NULL,
  PRIMARY KEY (user_id, slug),
  CONSTRAINT fk_presets_user FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;
