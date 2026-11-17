-- users 테이블 정의. ./minidb init schema/users.sql 로 data/users.tbl 을 만든다.
-- 첫 컬럼은 항상 id INT (INSERT 할 때 자동 부여).
CREATE TABLE users (id INT, name VARCHAR(32), age INT);
