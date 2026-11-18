-- 오류가 나도 다음 문장은 계속 실행되고, 실패한 INSERT 는 id 를 쓰지 않는다
INSERT INTO users (name) VALUES ('first');
SELECT * FORM users;
DELETE FROM users;
INSERT INTO users (id, name) VALUES (100, 'hack');
INSERT INTO users (name) VALUES ('this-name-is-definitely-longer-than-32-bytes');
INSERT INTO users (age) VALUES ('twenty');
INSERT INTO users VALUES ('only-two', 'values');
SELECT phone FROM users;
SELECT * FROM missing_table;
INSERT INTO users (name) VALUES ('it''s ok');
SELECT id, name FROM users;
