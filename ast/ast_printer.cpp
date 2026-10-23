#include "ast_printer.h"

const std::string ASTPrinter::INDENT_STR = "  ";

void ASTPrinter::indent() {
    for (int i = 0; i < indentLevel; i++) {
        std::cout << INDENT_STR;
    }
}

void ASTPrinter::println(const std::string& text) {
    indent();
    std::cout << text << std::endl;
}

void ASTPrinter::print(std::shared_ptr<Program> program) {
    indentLevel = 0;
    program->accept(this);
}

void ASTPrinter::visit(Program* node) {
    println("Program");
    indentLevel++;
    for (auto& decl : node->declarations) {
        decl->accept(this);
    }
    indentLevel--;
}

void ASTPrinter::printTypeParams(const std::vector<std::string>& typeParams,
                                 const std::map<std::string, std::vector<std::string>>& constraints) {
    if (typeParams.empty()) return;
    println("TypeParams:");
    indentLevel++;
    for (const auto& tp : typeParams) {
        auto it = constraints.find(tp);
        if (it != constraints.end() && !it->second.empty()) {
            std::string s = tp + ": ";
            for (size_t i = 0; i < it->second.size(); ++i) {
                if (i) s += " + ";
                s += it->second[i];
            }
            println(s);
        } else {
            println(tp);
        }
    }
    indentLevel--;
}

void ASTPrinter::printParams(const std::vector<std::pair<std::string, std::string>>& params,
                             const std::vector<std::string>* defaults) {
    println("Parameters:");
    indentLevel++;
    for (size_t i = 0; i < params.size(); ++i) {
        bool hasDef = defaults && i < defaults->size() && !(*defaults)[i].empty();
        println(params[i].first + " " + params[i].second + (hasDef ? " = " + (*defaults)[i] : ""));
    }
    indentLevel--;
}

void ASTPrinter::visit(FunctionDecl* node) {
    println("FunctionDecl: " + node->name + " -> " + node->returnType +
            (node->isAsync ? " (async)" : "") + (node->mustUse ? " (must_use)" : ""));
    indentLevel++;

    printTypeParams(node->typeParams, node->constraints);
    printParams(node->params, &node->paramDefaultTexts);

    // A forward declaration (prototype) has no body — omit the section rather than
    // dereferencing a null body (cf. ReturnStmt skipping a null value).
    if (node->body) {
        println("Body:");
        indentLevel++;
        node->body->accept(this);
        indentLevel--;
    }

    indentLevel--;
}

void ASTPrinter::visit(VarDecl* node) {
    println("VarDecl: " + node->name + ": " + node->type);
    indentLevel++;
    if (node->initializer) {
        println("Initializer:");
        indentLevel++;
        node->initializer->accept(this);
        indentLevel--;
    }
    indentLevel--;
}

void ASTPrinter::visit(StructDecl* node) {
    println("StructDecl: " + node->name);
    indentLevel++;

    printTypeParams(node->typeParams, node->constraints);
    println("Fields:");
    indentLevel++;
    for (auto& field : layoutFields(node->fields, node->pads)) {
        std::string line = field.unnamed ? field.type : field.type + " " + field.name;
        if (field.bitWidth > 0 || field.unnamed) line += " : " + std::to_string(field.bitWidth);
        println(line);
        if (field.defaultValue) {
            indentLevel++;
            println("Default:");
            indentLevel++;
            field.defaultValue->accept(this);
            indentLevel -= 2;
        }
    }
    indentLevel--;

    if (!node->methods.empty()) {
        println("Methods:");
        indentLevel++;
        for (auto& m : node->methods) m->accept(this);
        indentLevel--;
    }

    indentLevel--;
}

void ASTPrinter::visit(ExternDecl* node) {
    println("ExternDecl: " + node->name + " -> " + node->returnType);
    indentLevel++;

    printParams(node->params);

    indentLevel--;
}

void ASTPrinter::visit(IntrinsicDecl* node) {
    println("IntrinsicDecl: " + node->name + " -> " + node->returnType);
    indentLevel++;

    printParams(node->params);

    indentLevel--;
}

void ASTPrinter::visit(BlockStmt* node) {
    println("BlockStmt");
    indentLevel++;

    for (auto& item : node->items) {
        if (std::holds_alternative<DeclPtr>(item)) {
            std::get<DeclPtr>(item)->accept(this);
        } else if (std::holds_alternative<StmtPtr>(item)) {
            std::get<StmtPtr>(item)->accept(this);
        }
    }

    indentLevel--;
}

void ASTPrinter::visit(IfStmt* node) {
    // An `else if` chain is printed with a loop, one level deeper per link.
    int base = indentLevel;
    for (IfStmt* n = node; n;) {
        println("IfStmt");
        indentLevel++;

        println("Condition:");
        indentLevel++;
        n->condition->accept(this);
        indentLevel--;

        println("Then:");
        indentLevel++;
        n->thenBranch->accept(this);
        indentLevel--;

        IfStmt* next = nullptr;
        if (n->elseBranch) {
            println("Else:");
            indentLevel++;
            next = dynamic_cast<IfStmt*>(n->elseBranch.get());
            if (!next) n->elseBranch->accept(this);
        }
        n = next;
    }
    indentLevel = base;
}

void ASTPrinter::visit(ForStmt* node) {
    println(node->label.empty() ? "ForStmt" : "ForStmt (label: " + node->label + ")");
    indentLevel++;

    if (node->init) {
        println("Init:");
        indentLevel++;
        node->init->accept(this);
        indentLevel--;
    }

    if (node->condition) {
        println("Condition:");
        indentLevel++;
        node->condition->accept(this);
        indentLevel--;
    }

    if (node->step) {
        println("Step:");
        indentLevel++;
        node->step->accept(this);
        indentLevel--;
    }

    println("Body:");
    indentLevel++;
    node->body->accept(this);
    indentLevel--;

    indentLevel--;
}

void ASTPrinter::visit(ForInStmt* node) {
    println("ForInStmt (" + node->varName + " in ...)" + (node->label.empty() ? "" : " (label: " + node->label + ")"));
    indentLevel++;
    println("Iterable:");
    indentLevel++;
    node->iterable->accept(this);
    indentLevel--;
    println("Body:");
    indentLevel++;
    node->body->accept(this);
    indentLevel--;
    indentLevel--;
}

void ASTPrinter::visit(WhileStmt* node) {
    println(node->label.empty() ? "WhileStmt" : "WhileStmt (label: " + node->label + ")");
    indentLevel++;

    println("Condition:");
    indentLevel++;
    node->condition->accept(this);
    indentLevel--;

    println("Body:");
    indentLevel++;
    node->body->accept(this);
    indentLevel--;

    indentLevel--;
}

void ASTPrinter::visit(DoWhileStmt* node) {
    println(node->label.empty() ? "DoWhileStmt" : "DoWhileStmt (label: " + node->label + ")");
    indentLevel++;
    println("Body:");
    indentLevel++;
    node->body->accept(this);
    indentLevel--;
    println("Condition:");
    indentLevel++;
    node->condition->accept(this);
    indentLevel--;
    indentLevel--;
}

void ASTPrinter::visit(ReturnStmt* node) {
    println("ReturnStmt");
    if (node->value) {
        indentLevel++;
        node->value->accept(this);
        indentLevel--;
    }
}

void ASTPrinter::visit(BreakStmt* node) {
    println(node->label.empty() ? "BreakStmt" : "BreakStmt " + node->label);
}

void ASTPrinter::visit(ExprStmt* node) {
    println("ExprStmt");
    indentLevel++;
    node->expr->accept(this);
    indentLevel--;
}

void ASTPrinter::visit(BinaryExpr* node) {
    // A left-leaning chain (`a + b + c ...`) is as deep as it is long: print its left
    // spine with a loop instead of one recursive call per operator.
    int base = indentLevel;
    std::vector<BinaryExpr*> spine;
    for (BinaryExpr* b = node; b; b = dynamic_cast<BinaryExpr*>(b->left.get())) {
        println("BinaryExpr: " + b->op);
        indentLevel++;
        println("Left:");
        indentLevel++;
        spine.push_back(b);
    }
    spine.back()->left->accept(this);
    for (size_t i = spine.size(); i-- > 0;) {
        indentLevel = base + 2 * (int)i + 1;
        println("Right:");
        indentLevel++;
        spine[i]->right->accept(this);
    }
    indentLevel = base;
}

void ASTPrinter::visit(UnaryExpr* node) {
    println("UnaryExpr: " + node->op);
    indentLevel++;
    node->operand->accept(this);
    indentLevel--;
}

void ASTPrinter::visit(IncDecExpr* node) {
    std::string tag = std::string(node->prefix ? "pre" : "post") + (node->decrement ? "--" : "++");
    println("IncDecExpr: " + tag);
    indentLevel++;
    node->operand->accept(this);
    indentLevel--;
}

void ASTPrinter::visit(QuestionExpr* node) {
    println("QuestionExpr (?)");
    indentLevel++;
    node->operand->accept(this);
    indentLevel--;
}

void ASTPrinter::visit(TernaryExpr* node) {
    println("TernaryExpr (?:)");
    indentLevel++;
    node->condition->accept(this);
    node->thenExpr->accept(this);
    node->elseExpr->accept(this);
    indentLevel--;
}

void ASTPrinter::visit(CallExpr* node) {
    println("CallExpr");
    indentLevel++;

    println("Callee:");
    indentLevel++;
    node->callee->accept(this);
    indentLevel--;

    println("Args:");
    indentLevel++;
    for (auto& arg : node->args) {
        arg->accept(this);
    }
    indentLevel--;

    indentLevel--;
}

void ASTPrinter::visit(IndexExpr* node) {
    println(node->highIndex ? "IndexExpr (slice)" : "IndexExpr");
    indentLevel++;

    println("Base:");
    indentLevel++;
    node->base->accept(this);
    indentLevel--;

    println("Index:");
    indentLevel++;
    node->index->accept(this);
    indentLevel--;

    if (node->highIndex) {
        println("High:");
        indentLevel++;
        node->highIndex->accept(this);
        indentLevel--;
    }

    indentLevel--;
}

void ASTPrinter::visit(MemberExpr* node) {
    println("MemberExpr: ." + node->member);
    indentLevel++;
    node->base->accept(this);
    indentLevel--;
}

void ASTPrinter::visit(CastExpr* node) {
    println("CastExpr: (" + node->targetType + ")");
    indentLevel++;
    node->expr->accept(this);
    indentLevel--;
}

void ASTPrinter::visit(LiteralExpr* node) {
    std::string kind;
    switch (node->kind) {
        case LiteralExpr::Kind::INT: kind = "INT"; break;
        case LiteralExpr::Kind::FLOAT: kind = "FLOAT"; break;
        case LiteralExpr::Kind::STRING: kind = "STRING"; break;
        case LiteralExpr::Kind::CHAR: kind = "CHAR"; break;
        case LiteralExpr::Kind::BOOL: kind = "BOOL"; break;
        case LiteralExpr::Kind::NULL_VAL: kind = "NULL"; break;
    }
    println("LiteralExpr(" + kind + "): " + node->value);
}

void ASTPrinter::visit(IdentExpr* node) {
    println("IdentExpr: " + node->name);
}

void ASTPrinter::visit(InterfaceDecl* node) {
    println("InterfaceDecl: " + node->name);
    indentLevel++;
    for (const auto& m : node->methods) {
        std::string sig = m.returnType + " " + m.name + "(";
        for (size_t i = 0; i < m.params.size(); ++i) {
            if (i) sig += ", ";
            sig += m.params[i].first;
        }
        sig += ")";
        println(sig);
    }
    indentLevel--;
}

void ASTPrinter::visit(ContinueStmt* node) { println(node->label.empty() ? "ContinueStmt" : "ContinueStmt " + node->label); }

void ASTPrinter::visit(MatchStmt* node) {
    println("MatchStmt");
    indentLevel++;
    println("Subject:");
    indentLevel++;
    node->subject->accept(this);
    indentLevel--;
    for (auto& arm : node->arms) {
        std::string label = arm.variant.empty() ? "_" : arm.variant;
        if (!arm.bindings.empty()) {
            label += "(";
            for (size_t i = 0; i < arm.bindings.size(); ++i) { if (i) label += ", "; label += arm.bindings[i]; }
            label += ")";
        }
        println("Arm " + label + ":");
        indentLevel++;
        if (arm.body) arm.body->accept(this);
        indentLevel--;
    }
    indentLevel--;
}

void ASTPrinter::visit(SwitchStmt* node) {
    println("SwitchStmt");
    indentLevel++;
    println("Subject:");
    indentLevel++;
    node->subject->accept(this);
    indentLevel--;
    for (auto& c : node->cases) {
        if (c.value) {
            println("Case:");
            indentLevel++;
            c.value->accept(this);
        } else {
            println("Default:");
            indentLevel++;
        }
        for (auto& it : c.stmts) {
            if (std::holds_alternative<DeclPtr>(it)) std::get<DeclPtr>(it)->accept(this);
            else std::get<StmtPtr>(it)->accept(this);
        }
        indentLevel--;
    }
    indentLevel--;
}

void ASTPrinter::visit(TemplateCallExpr* node) {
    std::string sig = node->templateName + "<";
    for (size_t i = 0; i < node->typeArgs.size(); ++i) {
        if (i) sig += ",";
        sig += node->typeArgs[i];
    }
    sig += ">";
    println("TemplateCallExpr: " + sig);
    indentLevel++;
    for (auto& a : node->args) a->accept(this);
    indentLevel--;
}

void ASTPrinter::visit(AllocWithExpr* node) {
    println("AllocWithExpr: alloc_with(<allocator>, " + node->elemType + ", ...)");
    indentLevel++;
    node->allocator->accept(this);
    node->count->accept(this);
    indentLevel--;
}

void ASTPrinter::visit(StructInitExpr* node) {
    println("StructInitExpr: " + node->structName);
    indentLevel++;
    for (const auto& [name, expr] : node->fieldInits) {
        if (!name.empty()) println("." + name + ":");
        indentLevel++;
        expr->accept(this);
        indentLevel--;
    }
    indentLevel--;
}

void ASTPrinter::visit(ArrayLitExpr* node) {
    println("ArrayLitExpr");
    indentLevel++;
    for (const auto& el : node->elements) el->accept(this);
    indentLevel--;
}

void ASTPrinter::visit(LambdaExpr* node) {
    std::string sig = node->returnType + "(";
    for (size_t i = 0; i < node->params.size(); ++i) {
        if (i) sig += ", ";
        sig += node->params[i].first + " " + node->params[i].second;
    }
    sig += ")";
    println("LambdaExpr: " + sig);
    if (node->body) { indentLevel++; node->body->accept(this); indentLevel--; }
}

void ASTPrinter::visit(AsmStmt* node) {
    println("AsmStmt: asm(\"" + node->asmString + "\")");
    if (!node->outputs.empty() || !node->inputs.empty()) {
        indentLevel++;
        for (const auto& [c, e] : node->outputs) {
            println("output: \"" + c + "\"");
            indentLevel++; e->accept(this); indentLevel--;
        }
        for (const auto& [c, e] : node->inputs) {
            println("input: \"" + c + "\"");
            indentLevel++; e->accept(this); indentLevel--;
        }
        indentLevel--;
    }
}

void ASTPrinter::visit(ThreadCreateExpr* node) {
    println("ThreadCreateExpr");
    indentLevel++; node->worker->accept(this); indentLevel--;
}

void ASTPrinter::visit(ThreadJoinStmt* node) {
    println("ThreadJoinStmt");
    indentLevel++; node->tid->accept(this); indentLevel--;
}

void ASTPrinter::visit(ThrowStmt* node) {
    println("ThrowStmt");
    if (node->value) { indentLevel++; node->value->accept(this); indentLevel--; }
}

void ASTPrinter::visit(TryStmt* node) {
    println("TryStmt");
    indentLevel++;
    if (node->body) node->body->accept(this);
    for (const auto& c : node->catches) {
        println("CatchClause: " + (c.type.empty() ? c.name : c.type + " " + c.name));
        indentLevel++;
        if (c.body) c.body->accept(this);
        indentLevel--;
    }
    if (node->finally) { println("Finally"); indentLevel++; node->finally->accept(this); indentLevel--; }
    indentLevel--;
}

void ASTPrinter::visit(DeferStmt* node) {
    println(node->isErr ? "DeferStmt (err)" : "DeferStmt");
    indentLevel++;
    if (node->body) node->body->accept(this);
    indentLevel--;
}

void ASTPrinter::visit(SizeofExpr* node) {
    if (node->operand) {
        println("SizeofExpr: sizeof(<expr>)");
        indentLevel++;
        node->operand->accept(this);
        indentLevel--;
        return;
    }
    println("SizeofExpr: sizeof(" + node->typeName + ")");
}

void ASTPrinter::visit(FreeClosureExpr* node) {
    println("FreeClosureExpr:");
    indentLevel++;
    if (node->closure) node->closure->accept(this);
    indentLevel--;
}

void ASTPrinter::visit(AwaitExpr* node) {
    println("AwaitExpr:");
    indentLevel++;
    if (node->operand) node->operand->accept(this);
    indentLevel--;
}

void ASTPrinter::visit(UnionDecl* node) {
    println("UnionDecl: " + node->name);
    indentLevel++;
    for (const auto& f : layoutFields(node->fields, node->pads))
        println(f.unnamed ? f.type + " : " + std::to_string(f.bitWidth) : f.type + " " + f.name);
    indentLevel--;
}

void ASTPrinter::visit(EnumDecl* node) {
    println("EnumDecl: " + node->name);
    indentLevel++;
    printTypeParams(node->typeParams, {});   // EnumDecl has no constraints field
    int sinceExpr = -1;   // members after a value expression: "<expr>+k"
    for (size_t i = 0; i < node->members.size(); ++i) {
        const ExprPtr* ve = i < node->valueExprs.size() && node->valueExprs[i] ? &node->valueExprs[i] : nullptr;
        std::string val = std::to_string(node->members[i].second);
        if (ve) { val = "<expr>"; sinceExpr = 0; }
        else if (sinceExpr >= 0) val = "<expr>+" + std::to_string(++sinceExpr);
        std::string line = node->members[i].first + " = " + val;
        if (i < node->payloads.size() && !node->payloads[i].empty()) {
            line += "(";
            for (size_t j = 0; j < node->payloads[i].size(); ++j) {
                if (j) line += ", ";
                line += node->payloads[i][j];
            }
            line += ")";
        }
        println(line);
        if (ve) {
            indentLevel++;
            (*ve)->accept(this);
            indentLevel--;
        }
    }
    indentLevel--;
}

void ASTPrinter::visit(TypeAliasDecl* node) {
    println("TypeAlias: " + node->name + " = " + node->aliased);
}
