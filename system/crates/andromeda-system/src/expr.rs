//! Rule conditions (docs/system-health.md §4.1): comparisons, `and`, `or`, `not`, `+ -`, numbers, strings, state
//! fields by dotted name and rule params by name. Types are checked when a rule loads, against the state schema.
//! Logic is three-valued: a field whose area is unavailable is unknown, and a condition that ends unknown is not met,
//! so neither `when` nor `clear` fires on missing data.

use std::cmp::Ordering;
use std::fmt;

/// The type of a state field or param. `Level` fields compare by their position in the list.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Type {
    Num,
    Bool,
    Str,
    Level(&'static [&'static str]),
}

#[derive(Clone, Debug, PartialEq)]
pub enum Value {
    Num(f64),
    Bool(bool),
    Str(String),
}

/// Names a rule may read: state fields (`battery.percent`) and the rule's own params (`low_pct`).
pub trait Schema {
    fn type_of(&self, name: &str) -> Option<Type>;
}

/// Current values; `None` = unknown (its area is unavailable).
pub trait Values {
    fn value_of(&self, name: &str) -> Option<Value>;
}

#[derive(Debug, PartialEq)]
pub struct Error(pub String);

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

fn err<T>(message: impl Into<String>) -> Result<T, Error> {
    Err(Error(message.into()))
}

#[derive(Clone, Copy, Debug, PartialEq)]
enum Cmp {
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
}

#[derive(Debug)]
enum Node {
    Num(f64),
    Bool(bool),
    Str(String),
    Name(String, Type),
    /// A string literal resolved to its position in a level list.
    Level(usize),
    Add(Box<Node>, Box<Node>),
    Sub(Box<Node>, Box<Node>),
    Cmp(Cmp, Box<Node>, Box<Node>),
    Not(Box<Node>),
    And(Box<Node>, Box<Node>),
    Or(Box<Node>, Box<Node>),
}

/// A checked condition, ready to evaluate.
#[derive(Debug)]
pub struct Expr {
    root: Node,
}

#[derive(Clone, Debug, PartialEq)]
enum Token {
    Num(f64),
    Str(String),
    Ident(String),
    Op(&'static str),
    Open,
    Close,
}

fn tokens(text: &str) -> Result<Vec<Token>, Error> {
    let mut out = Vec::new();
    let chars: Vec<char> = text.chars().collect();
    let mut i = 0;
    while i < chars.len() {
        let c = chars[i];
        if c.is_whitespace() {
            i += 1;
        } else if c.is_ascii_digit() {
            let start = i;
            while i < chars.len() && (chars[i].is_ascii_digit() || chars[i] == '.') {
                i += 1;
            }
            let s: String = chars[start..i].iter().collect();
            out.push(Token::Num(s.parse().map_err(|_| Error(format!("bad number {s}")))?));
        } else if c == '\'' {
            let start = i + 1;
            i = start;
            while i < chars.len() && chars[i] != '\'' {
                i += 1;
            }
            if i == chars.len() {
                return err("unterminated string");
            }
            out.push(Token::Str(chars[start..i].iter().collect()));
            i += 1;
        } else if c.is_ascii_alphabetic() || c == '_' {
            let start = i;
            while i < chars.len() && (chars[i].is_ascii_alphanumeric() || chars[i] == '_' || chars[i] == '.') {
                i += 1;
            }
            out.push(Token::Ident(chars[start..i].iter().collect()));
        } else if c == '(' {
            out.push(Token::Open);
            i += 1;
        } else if c == ')' {
            out.push(Token::Close);
            i += 1;
        } else {
            let two: String = chars[i..chars.len().min(i + 2)].iter().collect();
            let op = ["==", "!=", "<=", ">="].into_iter().find(|op| two == *op);
            if let Some(op) = op {
                out.push(Token::Op(op));
                i += 2;
            } else if let Some(op) = ["<", ">", "+", "-"].into_iter().find(|op| op.starts_with(c)) {
                out.push(Token::Op(op));
                i += 1;
            } else {
                return err(format!("unexpected {c:?}"));
            }
        }
    }
    Ok(out)
}

struct Parser<'a> {
    tokens: Vec<Token>,
    at: usize,
    schema: &'a dyn Schema,
}

impl Parser<'_> {
    fn peek(&self) -> Option<&Token> {
        self.tokens.get(self.at)
    }

    fn keyword(&mut self, word: &str) -> bool {
        if self.peek() == Some(&Token::Ident(word.to_owned())) {
            self.at += 1;
            true
        } else {
            false
        }
    }

    fn or(&mut self) -> Result<Node, Error> {
        let mut left = self.and()?;
        while self.keyword("or") {
            left = Node::Or(Box::new(bool_node(left)?), Box::new(bool_node(self.and()?)?));
        }
        Ok(left)
    }

    fn and(&mut self) -> Result<Node, Error> {
        let mut left = self.not()?;
        while self.keyword("and") {
            left = Node::And(Box::new(bool_node(left)?), Box::new(bool_node(self.not()?)?));
        }
        Ok(left)
    }

    fn not(&mut self) -> Result<Node, Error> {
        if self.keyword("not") {
            return Ok(Node::Not(Box::new(bool_node(self.not()?)?)));
        }
        self.cmp()
    }

    fn cmp(&mut self) -> Result<Node, Error> {
        let left = self.sum()?;
        let op = match self.peek() {
            Some(Token::Op("==")) => Cmp::Eq,
            Some(Token::Op("!=")) => Cmp::Ne,
            Some(Token::Op("<")) => Cmp::Lt,
            Some(Token::Op("<=")) => Cmp::Le,
            Some(Token::Op(">")) => Cmp::Gt,
            Some(Token::Op(">=")) => Cmp::Ge,
            _ => return Ok(left),
        };
        self.at += 1;
        let right = self.sum()?;
        compare(op, left, right)
    }

    fn sum(&mut self) -> Result<Node, Error> {
        let mut left = self.atom()?;
        loop {
            let add = match self.peek() {
                Some(Token::Op("+")) => true,
                Some(Token::Op("-")) => false,
                _ => return Ok(left),
            };
            self.at += 1;
            let right = self.atom()?;
            if type_of(&left) != Some(Type::Num) || type_of(&right) != Some(Type::Num) {
                return err("+ and - take numbers");
            }
            left = if add {
                Node::Add(Box::new(left), Box::new(right))
            } else {
                Node::Sub(Box::new(left), Box::new(right))
            };
        }
    }

    fn atom(&mut self) -> Result<Node, Error> {
        let token = self.peek().cloned();
        self.at += 1;
        match token {
            Some(Token::Num(n)) => Ok(Node::Num(n)),
            Some(Token::Str(s)) => Ok(Node::Str(s)),
            Some(Token::Ident(word)) if word == "true" || word == "false" => Ok(Node::Bool(word == "true")),
            Some(Token::Ident(name)) => match self.schema.type_of(&name) {
                Some(t) => Ok(Node::Name(name, t)),
                None => err(format!("unknown name {name}")),
            },
            Some(Token::Open) => {
                let inner = self.or()?;
                if self.peek() != Some(&Token::Close) {
                    return err("missing )");
                }
                self.at += 1;
                Ok(inner)
            }
            other => err(format!("unexpected {other:?}")),
        }
    }
}

fn type_of(node: &Node) -> Option<Type> {
    match node {
        Node::Num(_) | Node::Add(..) | Node::Sub(..) => Some(Type::Num),
        Node::Bool(_) | Node::Cmp(..) | Node::Not(_) | Node::And(..) | Node::Or(..) => Some(Type::Bool),
        Node::Str(_) => Some(Type::Str),
        Node::Name(_, t) => Some(*t),
        Node::Level(_) => None,
    }
}

fn bool_node(node: Node) -> Result<Node, Error> {
    if type_of(&node) == Some(Type::Bool) { Ok(node) } else { err("and, or and not take conditions") }
}

/// Checks the operand types; a string literal next to a level field becomes its position in the level list.
fn compare(op: Cmp, left: Node, right: Node) -> Result<Node, Error> {
    let level_of = |node: &Node| match type_of(node) {
        Some(Type::Level(levels)) => Some(levels),
        _ => None,
    };
    let resolve = |levels: &'static [&'static str], node: Node| match node {
        Node::Str(s) => match levels.iter().position(|l| *l == s) {
            Some(i) => Ok(Node::Level(i)),
            None => err(format!("{s:?} is not one of {levels:?}")),
        },
        other if level_of(&other) == Some(levels) => Ok(other),
        _ => err("a level compares with one of its names"),
    };
    let (left, right) = match (level_of(&left), level_of(&right)) {
        (Some(levels), _) => (left, resolve(levels, right)?),
        (None, Some(levels)) => (resolve(levels, left)?, right),
        (None, None) => {
            let (l, r) = (type_of(&left), type_of(&right));
            if l != r {
                return err(format!("cannot compare {l:?} with {r:?}"));
            }
            if l != Some(Type::Num) && !matches!(op, Cmp::Eq | Cmp::Ne) {
                return err("only numbers and levels have an order");
            }
            (left, right)
        }
    };
    Ok(Node::Cmp(op, Box::new(left), Box::new(right)))
}

impl Expr {
    /// Parses and type-checks `text`; the result must be a condition.
    pub fn parse(text: &str, schema: &dyn Schema) -> Result<Self, Error> {
        let mut parser = Parser { tokens: tokens(text)?, at: 0, schema };
        let root = bool_node(parser.or()?)?;
        if parser.at != parser.tokens.len() {
            return err(format!("unexpected {:?}", parser.tokens[parser.at]));
        }
        Ok(Self { root })
    }

    /// `true` only when the condition is known to hold.
    pub fn holds(&self, values: &dyn Values) -> bool {
        matches!(eval(&self.root, values), Some(Value::Bool(true)))
    }

    /// The names the condition reads, in order of first use.
    pub fn names(&self) -> Vec<&str> {
        fn walk<'a>(node: &'a Node, out: &mut Vec<&'a str>) {
            match node {
                Node::Name(name, _) if !out.contains(&name.as_str()) => out.push(name),
                Node::Add(a, b) | Node::Sub(a, b) | Node::Cmp(_, a, b) | Node::And(a, b) | Node::Or(a, b) => {
                    walk(a, out);
                    walk(b, out);
                }
                Node::Not(a) => walk(a, out),
                _ => {}
            }
        }
        let mut out = Vec::new();
        walk(&self.root, &mut out);
        out
    }
}

/// `None` = unknown. `and` and `or` follow Kleene logic: `false and unknown` is false, `true or unknown` is true.
fn eval(node: &Node, values: &dyn Values) -> Option<Value> {
    let num = |n: &Node| match eval(n, values) {
        Some(Value::Num(v)) => Some(v),
        _ => None,
    };
    let truth = |n: &Node| match eval(n, values) {
        Some(Value::Bool(b)) => Some(b),
        _ => None,
    };
    match node {
        Node::Num(n) => Some(Value::Num(*n)),
        Node::Bool(b) => Some(Value::Bool(*b)),
        Node::Str(s) => Some(Value::Str(s.clone())),
        #[allow(clippy::cast_precision_loss)] // level lists have a few entries
        Node::Level(i) => Some(Value::Num(*i as f64)),
        Node::Name(name, t) => {
            let v = values.value_of(name)?;
            match (t, v) {
                #[allow(clippy::cast_precision_loss)] // level lists have a few entries
                (Type::Level(levels), Value::Str(s)) => {
                    levels.iter().position(|l| *l == s).map(|i| Value::Num(i as f64))
                }
                (Type::Level(_), _) => None,
                (_, v) => Some(v),
            }
        }
        Node::Add(a, b) => Some(Value::Num(num(a)? + num(b)?)),
        Node::Sub(a, b) => Some(Value::Num(num(a)? - num(b)?)),
        Node::Cmp(op, a, b) => {
            let (a, b) = (eval(a, values)?, eval(b, values)?);
            let (Value::Num(x), Value::Num(y)) = (&a, &b) else {
                let equal = a == b;
                return match op {
                    Cmp::Eq => Some(Value::Bool(equal)),
                    Cmp::Ne => Some(Value::Bool(!equal)),
                    _ => None,
                };
            };
            let ordering = x.partial_cmp(y)?;
            Some(Value::Bool(match op {
                Cmp::Eq => ordering == Ordering::Equal,
                Cmp::Ne => ordering != Ordering::Equal,
                Cmp::Lt => ordering == Ordering::Less,
                Cmp::Le => ordering != Ordering::Greater,
                Cmp::Gt => ordering == Ordering::Greater,
                Cmp::Ge => ordering != Ordering::Less,
            }))
        }
        Node::Not(a) => truth(a).map(|b| Value::Bool(!b)),
        Node::And(a, b) => match (truth(a), truth(b)) {
            (Some(false), _) | (_, Some(false)) => Some(Value::Bool(false)),
            (Some(true), Some(true)) => Some(Value::Bool(true)),
            _ => None,
        },
        Node::Or(a, b) => match (truth(a), truth(b)) {
            (Some(true), _) | (_, Some(true)) => Some(Value::Bool(true)),
            (Some(false), Some(false)) => Some(Value::Bool(false)),
            _ => None,
        },
    }
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use std::collections::HashMap;

    use super::{Expr, Schema, Type, Value, Values};

    const WARNINGS: &[&str] = &["unknown", "none", "discharging", "low", "critical", "action"];

    struct Fixture(HashMap<&'static str, Option<Value>>);

    impl Schema for Fixture {
        fn type_of(&self, name: &str) -> Option<Type> {
            match name {
                "power.on_battery" => Some(Type::Bool),
                "battery.percent" | "low_pct" | "margin_pct" => Some(Type::Num),
                "battery.warning" => Some(Type::Level(WARNINGS)),
                _ => None,
            }
        }
    }

    impl Values for Fixture {
        fn value_of(&self, name: &str) -> Option<Value> {
            self.0.get(name).cloned().flatten()
        }
    }

    fn state(on_battery: Option<bool>, percent: Option<f64>, warning: &str) -> Fixture {
        Fixture(HashMap::from([
            ("power.on_battery", on_battery.map(Value::Bool)),
            ("battery.percent", percent.map(Value::Num)),
            ("battery.warning", Some(Value::Str(warning.to_owned()))),
            ("low_pct", Some(Value::Num(20.0))),
            ("margin_pct", Some(Value::Num(5.0))),
        ]))
    }

    const WHEN: &str = "power.on_battery and (battery.percent <= low_pct or battery.warning >= 'low')";
    const CLEAR: &str = "not power.on_battery or battery.percent >= low_pct + margin_pct";

    #[test]
    fn low_power_plan_conditions() {
        let schema = state(None, None, "none");
        let (when, clear) = (Expr::parse(WHEN, &schema).unwrap(), Expr::parse(CLEAR, &schema).unwrap());
        assert!(when.holds(&state(Some(true), Some(18.0), "none")));
        assert!(when.holds(&state(Some(true), Some(40.0), "critical")));
        assert!(!when.holds(&state(Some(false), Some(10.0), "low")));
        assert!(!clear.holds(&state(Some(true), Some(22.0), "none")));
        assert!(clear.holds(&state(Some(true), Some(25.0), "none")));
    }

    #[test]
    fn unknown_fields_neither_fire_nor_clear() {
        let schema = state(None, None, "none");
        let (when, clear) = (Expr::parse(WHEN, &schema).unwrap(), Expr::parse(CLEAR, &schema).unwrap());
        assert!(!when.holds(&state(None, Some(5.0), "low")));
        assert!(!clear.holds(&state(None, Some(5.0), "low")));
        // Kleene: a known true side of `or` decides even when the other side is unknown.
        assert!(clear.holds(&state(Some(false), None, "none")));
    }

    #[test]
    fn load_time_errors() {
        let schema = state(None, None, "none");
        for (text, wanted) in [
            ("battery.warning >= 'empty'", "is not one of"),
            ("battery.percent == true", "cannot compare"),
            ("battery.nope > 1", "unknown name"),
            ("battery.percent + 1", "take conditions"),
            ("power.on_battery and", "unexpected"),
        ] {
            let error = Expr::parse(text, &schema).unwrap_err();
            assert!(error.0.contains(wanted), "{text}: {error}");
        }
    }
}
