-- Inventory schema and a couple of reporting queries.
-- Demo file for Code Annotator.

CREATE TABLE products (
    id          INTEGER PRIMARY KEY,
    sku         VARCHAR(32) NOT NULL UNIQUE,
    name        TEXT NOT NULL,
    unit_price  DECIMAL(10, 2) NOT NULL CHECK (unit_price >= 0),
    created_at  TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE stock_moves (
    id          INTEGER PRIMARY KEY,
    product_id  INTEGER NOT NULL REFERENCES products(id),
    quantity    INTEGER NOT NULL,          -- negative = outbound
    moved_at    TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Current stock level per product
SELECT p.sku,
       p.name,
       COALESCE(SUM(m.quantity), 0) AS on_hand
FROM products p
LEFT JOIN stock_moves m ON m.product_id = p.id
GROUP BY p.sku, p.name
ORDER BY on_hand DESC;

-- Products that moved in the last 30 days
SELECT DISTINCT p.sku, p.name
FROM products p
JOIN stock_moves m ON m.product_id = p.id
WHERE m.moved_at >= DATETIME('now', '-30 days');
